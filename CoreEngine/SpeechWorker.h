/**
 * @file SpeechWorker.h
 * @brief Background execution manager for provider speech synthesis iterations.
 */

#pragma once

#include <vector>
#include <string>
#include "provider_abi.h"

class CSapiEngine;
class ProviderWrapper;

/**
 * @class SpeechWorker
 * @brief Encapsulates background worker thread lifecycle and C-ABI callback marshaling.
 */
class SpeechWorker
{
public:
    SpeechWorker(CSapiEngine* pEngine, ProviderWrapper* pWrapper);
    ~SpeechWorker();

    SpeechWorker(const SpeechWorker&) = delete;
    SpeechWorker& operator=(const SpeechWorker&) = delete;

    void Start(std::vector<char16_t> backingBuffer, std::vector<ProviderSpeechFragment> fragments);
    void Stop();

    /**
     * @brief Synchronously waits for worker thread termination.
     * @note Crucial teardown guardrail: prevents unmapped memory execution crashes (0xC0000005)
     *       when the provider HMODULE is unloaded by ProviderWrapper.
     */
    void WaitUntilFinished();
    
    bool IsFinished() const { return !m_isRunning; }

    static bool __stdcall AudioCallback(const uint8_t* pAudioBytes, uint32_t byteCount, void* ctx);
    static void __stdcall MetaCallback(const ProviderSpeechEvent* pEvent, void* ctx);

private:
    void ThreadProc(std::vector<char16_t> backingBuffer, std::vector<ProviderSpeechFragment> fragments);

    CSapiEngine* m_pEngine;
    ProviderWrapper* m_pWrapper;

    std::thread m_thread;
    volatile uint32_t m_abortFlag = 0;
    std::atomic<bool> m_isRunning{false};
};


