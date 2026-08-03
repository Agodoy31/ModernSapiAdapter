#pragma once

#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include "PipeClient.h"

class CSapiEngine;

class SpeechWorker
{
public:
    SpeechWorker(CSapiEngine* pEngine, std::shared_ptr<PipeClient> pClient);
    ~SpeechWorker();

    SpeechWorker(const SpeechWorker&) = delete;
    SpeechWorker& operator=(const SpeechWorker&) = delete;

    void Start(void* pSite); // pSite is ignored in V2, engine tracks it
    void Stop();

    void WaitUntilFinished();

private:
    void AudioThreadProc();
    void ControlThreadProc();

    CSapiEngine* m_pEngine;
    std::shared_ptr<PipeClient> m_pClient;

    std::thread m_audioThread;
    std::thread m_controlThread;
    std::atomic<bool> m_exit;
    std::atomic<bool> m_isSpeaking;
};
