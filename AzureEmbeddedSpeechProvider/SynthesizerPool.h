/**
 * @file SynthesizerPool.h
 * @brief Manages global EmbeddedSpeechConfig and per-request SpeechSynthesizer creation.
 */

#pragma once

#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include "provider_abi.h"
#include "PcmCache.h"

class AudioStreamHandler : public Microsoft::CognitiveServices::Speech::Audio::PushAudioOutputStreamCallback {
public:
    AudioStreamHandler();

    int Write(uint8_t* dataBuffer, uint32_t size) override;
    void OnWordBoundary(const Microsoft::CognitiveServices::Speech::SpeechSynthesisWordBoundaryEventArgs& e);
    void OnBookmarkReached(const Microsoft::CognitiveServices::Speech::SpeechSynthesisBookmarkEventArgs& e);
    void Close() override {}

    void AttachContext(const ProviderSpeakParams* params, std::vector<std::pair<uint32_t, uint32_t>> offsetMap, bool enableCaching, const CacheKey& cacheKey);
    void DetachContext(bool wasCancelled, bool allowShadowCache = false);
    void FinalizeShadowCache();

private:
    std::mutex m_contextMutex;
    const ProviderSpeakParams* m_params{nullptr};
    std::vector<std::pair<uint32_t, uint32_t>> m_offsetMap;
    bool m_hasEncounteredAudio{false};
    bool m_isShadowCaching{false};
    size_t m_leadingOffsetBytes{0};

    bool m_isCaching{false};

    void CorrectCacheOffsets();
    CacheKey m_cacheKey;
    CachePayload m_cachePayload;
};

class SynthesizerPool {
public:
    static void Shutdown();

    static std::shared_ptr<Microsoft::CognitiveServices::Speech::SpeechSynthesizer> CreateSynthesizer(
        const std::wstring& voiceName,
        std::shared_ptr<AudioStreamHandler> streamHandler);

private:
    static void Initialize();
    static std::shared_ptr<Microsoft::CognitiveServices::Speech::EmbeddedSpeechConfig> CreateConfig();

    static std::mutex s_mutex;
    static std::shared_ptr<Microsoft::CognitiveServices::Speech::EmbeddedSpeechConfig> s_config;
};
