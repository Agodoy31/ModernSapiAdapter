/**
 * @file SpeechWorker.h
 * @brief Manages background threads for reading audio streams and JSON events from PipeClient.
 */

#pragma once

#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include "PipeClient.h"

class CSapiEngine;

/**
 * @class SpeechWorker
 * @brief Coordinates Audio and Control thread loops to process provider data asynchronously.
 */
class SpeechWorker
{
public:
    /**
     * @brief Constructs a new SpeechWorker associated with a CSapiEngine and PipeClient.
     * @param pEngine Pointer to parent CSapiEngine instance.
     * @param pClient Shared pointer to PipeClient IPC transport.
     */
    SpeechWorker(CSapiEngine* pEngine, std::shared_ptr<PipeClient> pClient);

    /**
     * @brief Destructs SpeechWorker, stopping and joining background worker threads.
     */
    ~SpeechWorker();

    SpeechWorker(const SpeechWorker&) = delete;
    SpeechWorker& operator=(const SpeechWorker&) = delete;

    /**
     * @brief Starts background worker thread operations for a speech synthesis request.
     * @param pSite Opaque output site pointer (tracked internally by engine).
     * @param speakId The unique identifier for this speech request.
     */
    void Start(void* pSite, uint64_t speakId);

    /**
     * @brief Signals worker threads to stop and cancels pending I/O.
     */
    void Stop();

    /**
     * @brief Blocks calling thread until current speech synthesis completion is signaled.
     */
    void WaitUntilFinished();

private:
    /**
     * @brief Thread procedure for reading PCM audio bytes and dispatching to engine.
     */
    void AudioThreadProc();

    /**
     * @brief Thread procedure for reading JSON events and dispatching to engine.
     */
    void ControlThreadProc();

    CSapiEngine* m_pEngine;                  /**< Pointer to parent SAPI engine. */
    std::shared_ptr<PipeClient> m_pClient;   /**< Shared IPC PipeClient. */

    std::thread m_audioThread;               /**< Audio streaming worker thread. */
    std::thread m_controlThread;             /**< Control event worker thread. */
    std::atomic<bool> m_exit;                /**< Flag indicating worker shutdown. */
    std::atomic<bool> m_isSpeaking;          /**< Flag indicating active speech synthesis. */
    std::atomic<uint64_t> m_activeSpeakId{0};/**< Track the active speak_id for cancellation. */
};
