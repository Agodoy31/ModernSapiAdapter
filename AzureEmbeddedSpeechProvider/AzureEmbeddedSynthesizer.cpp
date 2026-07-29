#include "pch.h"
#include "AzureEmbeddedSynthesizer.h"
#include "SynthesizerPool.h"
#include "Logger.h"
#include "../SapiSsmlParser.Cpp/SapiSsmlParser.h"
#include <algorithm>
#include <future>
#include <atomic>

using namespace Microsoft::CognitiveServices::Speech;

bool AzureEmbeddedSynthesizer::Speak(const ProviderSpeakParams* params) {
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

    auto streamHandler = std::make_shared<AudioStreamHandler>(params, std::move(parseResult.OffsetMap));
    std::shared_ptr<SpeechSynthesizer> synth;

    try {
        synth = SynthesizerPool::CreateSynthesizer(voiceName, streamHandler);
    } catch (const std::exception& e) {
        LogError("Failed to create synthesizer for %ls: %s", voiceName.c_str(), e.what());
        return false;
    }

    if (!synth) return false;

    auto synthesisStarted = std::make_shared<std::atomic<bool>>(false);
    synth->SynthesisStarted += [synthesisStarted](const SpeechSynthesisEventArgs&) {
        synthesisStarted->store(true, std::memory_order_relaxed);
    };

    synth->WordBoundary += [streamHandler](const SpeechSynthesisWordBoundaryEventArgs& e) {
        streamHandler->OnWordBoundary(e);
    };

    synth->BookmarkReached += [streamHandler](const SpeechSynthesisBookmarkEventArgs& e) {
        streamHandler->OnBookmarkReached(e);
    };

    try {
        auto future = synth->SpeakSsmlAsync(ssmlUtf8);
        while (future.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout) {
            if (params->pAbortFlag && *params->pAbortFlag) {
                LogInfo("AzureEmbeddedSynthesizer::Speak abort flag detected, performing fire-and-forget cancellation.");
                streamHandler->DetachContext();
                std::thread([synth, synthesisStarted, f = std::move(future)]() mutable {
                    while (synthesisStarted && !synthesisStarted->load(std::memory_order_relaxed)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    try {
                        synth->StopSpeakingAsync().wait();
                    } catch (...) {}
                    try {
                        f.wait();
                    } catch (...) {}
                }).detach();
                return true;
            }
        }

        auto result = future.get();
        streamHandler->DetachContext();
        if (result && result->Reason == ResultReason::Canceled) {
            auto cancellation = SpeechSynthesisCancellationDetails::FromResult(result);
            LogError("Synthesis Canceled: %s", cancellation->ErrorDetails.c_str());
            return false;
        }
    } catch (const std::exception& e) {
        LogError("SpeakSsmlAsync threw: %s", e.what());
        return false;
    }
    
    return true;
}
