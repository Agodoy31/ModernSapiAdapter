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

class AudioStreamHandler : public Microsoft::CognitiveServices::Speech::Audio::PushAudioOutputStreamCallback {
public:
    AudioStreamHandler();

    int Write(uint8_t* dataBuffer, uint32_t size) override;
    void OnWordBoundary(const Microsoft::CognitiveServices::Speech::SpeechSynthesisWordBoundaryEventArgs& e);
    void OnBookmarkReached(const Microsoft::CognitiveServices::Speech::SpeechSynthesisBookmarkEventArgs& e);
    void Close() override {}

    void AttachContext(const ProviderSpeakParams* params, std::vector<std::pair<uint32_t, uint32_t>> offsetMap);
    void DetachContext(bool wasCancelled);

private:
    std::mutex m_contextMutex;
    const ProviderSpeakParams* m_params{nullptr};
    std::vector<std::pair<uint32_t, uint32_t>> m_offsetMap;
    bool m_hasEncounteredAudio{false};
    size_t m_leadingOffsetBytes{0};

};

struct PooledSynthesizer {
    std::shared_ptr<Microsoft::CognitiveServices::Speech::SpeechSynthesizer> synth;
    std::shared_ptr<AudioStreamHandler> streamHandler;
    std::wstring voiceName;
    bool isBusy{false};
};

class SynthesizerPool {
public:
    static void Shutdown();

    static std::shared_ptr<PooledSynthesizer> AcquireSynthesizer(
        const std::wstring& voiceName,
        const volatile uint32_t* pAbortFlag);

    static void ReleaseSynthesizer(std::shared_ptr<PooledSynthesizer> pooledSynth);



private:
    static void Initialize();
    static std::shared_ptr<Microsoft::CognitiveServices::Speech::EmbeddedSpeechConfig> CreateConfig();

    static std::mutex s_mutex;
    static std::shared_ptr<Microsoft::CognitiveServices::Speech::EmbeddedSpeechConfig> s_config;

    static std::mutex s_poolMutex;
    static std::condition_variable s_poolCondition;
    static std::vector<std::shared_ptr<PooledSynthesizer>> s_pool;
};
