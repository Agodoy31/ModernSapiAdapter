#include "pch.h"
#include "SynthesizerPool.h"
#include "ConfigParser.h"
#include "Logger.h"
#include "PcmTrimmer.h"

using namespace Microsoft::CognitiveServices::Speech;
using namespace winrt::Windows::Management::Deployment;

std::mutex SynthesizerPool::s_mutex;
std::shared_ptr<EmbeddedSpeechConfig> SynthesizerPool::s_config;

static std::string WStringToUTF8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), result.data(), size, nullptr, nullptr);
    return result;
}

static uint32_t ReverseMapOffset(uint32_t ssmlOffset, const std::vector<std::pair<uint32_t, uint32_t>>& offsetMap) {
    if (offsetMap.empty()) return 0;
    auto it = std::upper_bound(offsetMap.begin(), offsetMap.end(),
        ssmlOffset, [](uint32_t val, const std::pair<uint32_t, uint32_t>& item) {
            return val < item.first;
        });
    if (it != offsetMap.begin()) {
        --it;
        return it->second + (ssmlOffset - it->first);
    }
    return 0;
}

AudioStreamHandler::AudioStreamHandler() {
}

void AudioStreamHandler::AttachContext(const ProviderSpeakParams* params, std::vector<std::pair<uint32_t, uint32_t>> offsetMap, bool enableCaching, const CacheKey& cacheKey) {
    std::lock_guard<std::mutex> lock(m_contextMutex);
    m_params = params;
    m_offsetMap = std::move(offsetMap);
    m_isCaching = enableCaching;
    m_cacheKey = cacheKey;
    m_hasEncounteredAudio = false;
    m_isShadowCaching = false;
    
    m_cachePayload.AudioData.clear();
    m_cachePayload.Events.clear();
    m_cachePayload.StringStore.clear();
}

void AudioStreamHandler::DetachContext(bool wasCancelled, bool allowShadowCache) {
    std::lock_guard<std::mutex> lock(m_contextMutex);
    m_params = nullptr;
    m_offsetMap.clear();

    if (wasCancelled && allowShadowCache && m_isCaching) {
        m_isShadowCaching = true;
        LogInfo("AudioStreamHandler: Entering Shadow Caching mode. Silently recording background audio.");
        return; // Don't put in cache yet! Wait for FinalizeShadowCache
    }

    if (m_isCaching && m_hasEncounteredAudio && !wasCancelled) {
        PcmCache::Put(m_cacheKey, std::move(m_cachePayload));
    }
}

void AudioStreamHandler::FinalizeShadowCache() {
    std::lock_guard<std::mutex> lock(m_contextMutex);
    if (m_isShadowCaching && m_hasEncounteredAudio) {
        LogInfo("AudioStreamHandler: Shadow Caching complete! Payload successfully persisted.");
        PcmCache::Put(m_cacheKey, std::move(m_cachePayload));
        m_isShadowCaching = false;
    }
}

int AudioStreamHandler::Write(uint8_t* dataBuffer, uint32_t size) {
    std::lock_guard<std::mutex> lock(m_contextMutex);
    if (!m_params && !m_isShadowCaching) return 0;

    static bool firstAudioChunk = true;
    if (firstAudioChunk) {
        LogInfo("AudioStreamHandler: Received first chunk of %u bytes from Azure SDK.", size);
        firstAudioChunk = false;
    }

    if (m_params && m_params->pAbortFlag && *m_params->pAbortFlag) {
        return 0; // if we're not detached yet but flagged
    }

    size_t leadingOffset = 0;
    if (!m_hasEncounteredAudio) {
        leadingOffset = PcmTrimmer::FindLeadingAudioOffset(dataBuffer, size, 5);
        if (leadingOffset < size) {
            m_hasEncounteredAudio = true;
        }
    }

    uint8_t* activeData = dataBuffer + leadingOffset;
    size_t activeSize = size - leadingOffset;

    if (activeSize > 0) {
        if (m_isCaching) {
            m_cachePayload.AudioData.insert(m_cachePayload.AudioData.end(), activeData, activeData + activeSize);
        }

        if (m_params && m_params->AudioCallback) {
            bool cont = m_params->AudioCallback(activeData, static_cast<uint32_t>(activeSize), m_params->UserContext);
            if (!cont) {
                return 0;
            }
        }
    }
    return size;
}

void AudioStreamHandler::OnWordBoundary(const SpeechSynthesisWordBoundaryEventArgs& e) {
    std::lock_guard<std::mutex> lock(m_contextMutex);
    if (!m_params && !m_isShadowCaching) return;

    bool skipCallback = (!m_params || !m_params->MetaCallback);

    ProviderSpeechEvent ev = {};
    ev.EventType = PROVIDER_EVENT_WORD_BOUNDARY;
    ev.TextOffset = ReverseMapOffset(e.TextOffset, m_offsetMap);
    ev.TextLength = e.WordLength;
    ev.AudioByteOffset = (e.AudioOffset * 48) / 10000;
    
    if (m_isCaching) {
        m_cachePayload.Events.push_back(ev);
    }
    
    if (!skipCallback) {
        m_params->MetaCallback(&ev, m_params->UserContext);
    }
}

void AudioStreamHandler::OnBookmarkReached(const SpeechSynthesisBookmarkEventArgs& e) {
    std::lock_guard<std::mutex> lock(m_contextMutex);
    if (!m_params && !m_isShadowCaching) return;

    bool skipCallback = (!m_params || !m_params->MetaCallback);

    ProviderSpeechEvent ev = {};
    ev.EventType = PROVIDER_EVENT_BOOKMARK;

    uint32_t originalOffset = 0;
    std::wstring bookmarkName;

    if (e.Text.find("OFFSET_") == 0) {
        size_t nextUnderscore = e.Text.find('_', 7);
        if (nextUnderscore != std::string::npos) {
            try {
                originalOffset = std::stoul(e.Text.substr(7, nextUnderscore - 7));
            } catch (...) {}

            std::string utf8Name = e.Text.substr(nextUnderscore + 1);
            int size = MultiByteToWideChar(CP_UTF8, 0, utf8Name.c_str(), -1, nullptr, 0);
            if (size > 0) {
                bookmarkName.resize(size - 1);
                MultiByteToWideChar(CP_UTF8, 0, utf8Name.c_str(), -1, bookmarkName.data(), size);
            }
        }
    }

    ev.TextOffset = originalOffset;
    ev.TextLength = 0;
    ev.AudioByteOffset = (e.AudioOffset * 48) / 10000;
    ev.Reserved = 0;
    
    if (!bookmarkName.empty()) {
        if (m_isCaching) {
            m_cachePayload.StringStore.push_back(std::u16string(reinterpret_cast<const char16_t*>(bookmarkName.c_str())));
            ev.StringData = m_cachePayload.StringStore.back().c_str();
        } else {
            ev.StringData = reinterpret_cast<const char16_t*>(bookmarkName.c_str());
        }
    } else {
        ev.StringData = nullptr;
    }
    
    if (m_isCaching) {
        m_cachePayload.Events.push_back(ev);
    }

    if (!skipCallback) {
        m_params->MetaCallback(&ev, m_params->UserContext);
    }
}

std::shared_ptr<EmbeddedSpeechConfig> SynthesizerPool::CreateConfig() {
    std::vector<std::string> paths;

    try {
        PackageManager packageManager;
        auto packages = packageManager.FindPackagesForUser(L"");
        for (auto package : packages) {
            std::wstring pkgName = package.Id().Name().c_str();
            if (pkgName.find(L"MicrosoftWindows.Voice.") == 0) {
                std::wstring path = package.InstalledLocation().Path().c_str();
                paths.push_back(WStringToUTF8(path));
            }
        }
    } catch (const winrt::hresult_error& e) {
        LogWarn("Failed to enumerate MSIX packages: 0x%08X", e.code());
    }

    std::string decryptionKey;

    ProviderConfig providerConfig = ConfigParser::LoadMergedConfig();
    for (const auto& p : providerConfig.ExtraVoicePaths) {
        paths.push_back(p);
    }
    decryptionKey = providerConfig.DecryptionKey;

    auto config = EmbeddedSpeechConfig::FromPaths(paths);
    config->SetSpeechSynthesisOutputFormat(SpeechSynthesisOutputFormat::Raw24Khz16BitMonoPcm);
    config->SetProperty(PropertyId::SpeechServiceResponse_SynthesisEventsSyncToAudio, "false");
    config->SetProperty(PropertyId::SpeechServiceResponse_RequestSentenceBoundary, "false");
    config->SetProperty(PropertyId::SpeechServiceResponse_RequestPunctuationBoundary, "false");
    
#ifdef _DEBUG
    config->SetProperty(PropertyId::Speech_LogFilename, "C:\\Users\\AndresGodoy\\AppData\\Local\\ModernSapiAdapter\\Logs\\AzureSpeechSDK_debug.log");
#endif
    
    if (!decryptionKey.empty()) {
        config->SetProperty("EmbeddedSpeech_DecryptionKey", decryptionKey);
    } else {
        config->SetProperty("EmbeddedSpeech_DecryptionKey", "ZCjZ7nHDSLvf4gpELteM4AnzaWUjTpn7UkV7D@vvksl0w1SNgon6d1905WANbktDc9S39oaA4r29HJNayXvTq8fJsq");
    }

    return config;
}

void SynthesizerPool::Initialize() {
    if (!s_config) {
        s_config = CreateConfig();
        LogInfo("SynthesizerPool initialized.");
    }
}

void SynthesizerPool::Shutdown() {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_config = nullptr;
    LogInfo("SynthesizerPool shutdown completed.");
}

std::shared_ptr<SpeechSynthesizer> SynthesizerPool::CreateSynthesizer(
    const std::wstring& voiceName,
    std::shared_ptr<AudioStreamHandler> streamHandler) {

    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_config) {
        Initialize();
    }

    std::string key = s_config->GetProperty("EmbeddedSpeech_DecryptionKey");
    s_config->SetSpeechSynthesisVoice(WStringToUTF8(voiceName), "Key:" + key);

    auto audioConfig = Audio::AudioConfig::FromStreamOutput(
        Audio::AudioOutputStream::CreatePushStream(streamHandler));

    auto synth = SpeechSynthesizer::FromConfig(s_config, audioConfig);
    LogInfo("SynthesizerPool: Created request-scoped synthesizer for voice: %ls", voiceName.c_str());

    return synth;
}
