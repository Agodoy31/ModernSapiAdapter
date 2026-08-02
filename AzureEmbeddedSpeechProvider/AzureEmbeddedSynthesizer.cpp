#include "pch.h"
#include "AzureEmbeddedSynthesizer.h"
#include "SynthesizerPool.h"
#include "Logger.h"
#include "../SapiSsmlParser.Cpp/SapiSsmlParser.h"
#include "PcmCache.h"
#include <algorithm>
#include <future>
#include <atomic>

using namespace Microsoft::CognitiveServices::Speech;

bool AzureEmbeddedSynthesizer::Speak(const ProviderSpeakParams* params) {
    GlobalLazyInit();
    if (!params || !params->VoiceModel) {
        LogError("AzureEmbeddedSynthesizer::Speak failed: params or VoiceModel is null.");
        return false;
    }

    std::wstring voiceName = reinterpret_cast<const wchar_t*>(params->VoiceModel);
    LogInfo("AzureEmbeddedSynthesizer::Speak requested for voice: %ls", voiceName.c_str());
    auto parseResult = SapiSsmlParser::Parse(params->Fragments, params->FragmentCount, L"en-US");

    if (!parseResult.HasSpeakableText) {
        LogInfo("AzureEmbeddedSynthesizer::Speak bypassing synthesis: no speakable text found (bookmark-only fragments).");
        if (params->MetaCallback) {
            for (uint32_t i = 0; i < params->FragmentCount; ++i) {
                const auto& frag = params->Fragments[i];
                if (frag.Action == PROVIDER_ACTION_BOOKMARK) {
                    ProviderSpeechEvent ev = {};
                    ev.EventType = PROVIDER_EVENT_BOOKMARK;
                    ev.TextOffset = frag.OriginalOffset;
                    ev.TextLength = 0;
                    ev.AudioByteOffset = 0; 
                    ev.StringData = frag.Text; // Forward the SAPI bookmark name directly!
                    params->MetaCallback(&ev, params->UserContext);
                }
            }
        }
        return true;
    }

    std::string ssmlUtf8;
    int size = WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<LPCWSTR>(parseResult.SsmlString.c_str()), 
                                   (int)parseResult.SsmlString.length(), nullptr, 0, nullptr, nullptr);
    ssmlUtf8.resize(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<LPCWSTR>(parseResult.SsmlString.c_str()), 
                        (int)parseResult.SsmlString.length(), ssmlUtf8.data(), size, nullptr, nullptr);

    LogInfo("Generated SSML: %s", ssmlUtf8.c_str());

    uint32_t totalRawChars = 0;
    for (uint32_t i = 0; i < params->FragmentCount; ++i) {
        totalRawChars += params->Fragments[i].TextLength;
    }
    
    bool isCachingCandidate = SynthesizerPool::IsCacheEnabled() && (totalRawChars > 0 && totalRawChars <= 48);
    CacheKey cacheKey = {};
    if (isCachingCandidate && params->FragmentCount > 0) {
        cacheKey.VoiceName = voiceName;
        cacheKey.Rate = (int)params->Fragments[0].Rate;
        cacheKey.Pitch = (int)params->Fragments[0].Pitch;
        cacheKey.Volume = (int)params->Fragments[0].Volume;
        cacheKey.Text = parseResult.SsmlString;

        std::shared_ptr<const CachePayload> payload;
        if (PcmCache::TryGet(cacheKey, payload)) {
            LogInfo("AzureEmbeddedSynthesizer::Speak Cache HIT for '%s'. Bypassing Azure SDK.", ssmlUtf8.c_str());
            
            size_t audioChunks = payload->AudioData.size() / 4096;
            size_t remainder = payload->AudioData.size() % 4096;
            size_t offset = 0;


            // Replay events safely: we'll fire events BEFORE the audio chunk that crosses their timestamp
            if (params->MetaCallback) {
                uint32_t currentAudioOffset = 0;
                size_t currentEventIdx = 0;
                
                while (offset < payload->AudioData.size()) {
                    if (params->pAbortFlag && *params->pAbortFlag) return true;

                    uint32_t chunkSize = (uint32_t)std::min<size_t>(4096, payload->AudioData.size() - offset);
                    
                    while (currentEventIdx < payload->Events.size() && 
                           payload->Events[currentEventIdx].AudioByteOffset <= currentAudioOffset + chunkSize) {
                        params->MetaCallback(&payload->Events[currentEventIdx], params->UserContext);
                        currentEventIdx++;
                    }

                    if (params->AudioCallback) {
                        if (!params->AudioCallback(payload->AudioData.data() + offset, chunkSize, params->UserContext)) {
                            return true; // Aborted by SAPI
                        }
                    }
                    offset += chunkSize;
                    currentAudioOffset += chunkSize;
                }
                
                // Fire any remaining trailing events
                while (currentEventIdx < payload->Events.size()) {
                    params->MetaCallback(&payload->Events[currentEventIdx], params->UserContext);
                    currentEventIdx++;
                }
            } else if (params->AudioCallback && !payload->AudioData.empty()) {
                size_t noMetaOffset = 0;
                while (noMetaOffset < payload->AudioData.size()) {
                    if (params->pAbortFlag && *params->pAbortFlag) return true;
                    uint32_t chunkSize = (uint32_t)std::min<size_t>(4096, payload->AudioData.size() - noMetaOffset);
                    if (!params->AudioCallback(payload->AudioData.data() + noMetaOffset, chunkSize, params->UserContext)) {
                        return true;
                    }
                    noMetaOffset += chunkSize;
                }
            }

            return true;
        }
        
        LogInfo("AzureEmbeddedSynthesizer::Speak Cache MISS for '%s'. Proceeding to synthesis.", ssmlUtf8.c_str());
    }

    std::shared_ptr<PooledSynthesizer> pooledSynth;

    try {
        pooledSynth = SynthesizerPool::AcquireSynthesizer(voiceName, params->pAbortFlag);
    } catch (const std::exception& e) {
        LogError("Failed to acquire synthesizer for %ls: %s", voiceName.c_str(), e.what());
        return false;
    }

    if (!pooledSynth) {
        // Aborted while waiting for an idle engine
        return true;
    }

    struct EngineGuard {
        std::shared_ptr<PooledSynthesizer> p;
        ~EngineGuard() { if (p) SynthesizerPool::ReleaseSynthesizer(p); }
        void detach() { p.reset(); }
    } guard{pooledSynth};

    auto synth = pooledSynth->synth;
    auto streamHandler = pooledSynth->streamHandler;

    streamHandler->AttachContext(params, std::move(parseResult.OffsetMap), isCachingCandidate, cacheKey);

    try {
        auto future = synth->SpeakSsmlAsync(ssmlUtf8);
        while (future.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout) {
            if (params->pAbortFlag && *params->pAbortFlag) {
                LogInfo("AzureEmbeddedSynthesizer::Speak abort flag detected, performing fire-and-forget cancellation.");
                
                streamHandler->DetachContext(true);
                
                std::thread([synth, f = std::move(future), pooledSynth]() mutable {
                    try {
                        synth->StopSpeakingAsync().wait();
                    } catch (...) {}
                    try {
                        f.wait();
                    } catch (...) {}

                    SynthesizerPool::ReleaseSynthesizer(pooledSynth);
                }).detach();
                
                guard.detach(); // Transfer responsibility to the detached thread
                return true;
            }
        }

        auto result = future.get();
        bool isCancelled = (result && result->Reason == ResultReason::Canceled);
        streamHandler->DetachContext(isCancelled);
        // guard automatically releases the synthesizer when the function returns
        
        if (isCancelled) {
            LogWarn("AzureEmbeddedSynthesizer::Speak synthesis was canceled natively.");
            auto cancellationDetails = SpeechSynthesisCancellationDetails::FromResult(result);
            if (cancellationDetails) {
                LogWarn("Cancellation Reason: %d", (int)cancellationDetails->Reason);
                if (cancellationDetails->Reason == CancellationReason::Error) {
                    LogWarn("Error Details: %s", cancellationDetails->ErrorDetails.c_str());
                }
            }
            return false;
        }

        return true;
    } catch (const std::exception& e) {
        LogError("SpeakSsmlAsync threw: %s", e.what());
        return false;
    }
    
    return true;
}
