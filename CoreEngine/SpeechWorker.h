/**
 * @file SpeechWorker.h
 * @brief Manages background threads for reading audio streams and JSON events from PipeClient.
 */

#pragma once

#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
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
     * @brief Starts background worker thread operations for a speech synthesis request when the pipe session is usable.
     * @param pSite Opaque output site pointer (tracked internally by engine).
     * @param speakId The unique identifier for this speech request.
     * @return True when the request entered the speaking state; false when the pipe session is quarantined.
     */
    bool Start(void* pSite, uint64_t speakId);

    /**
     * @brief Signals worker threads to stop and cancels pending I/O.
     */
    void Stop();

    /**
     * @brief Cancels the active request and waits until its committed PCM has been discarded.
     * @return S_OK after the provider acknowledges cancellation and the audio boundary is drained.
     */
    HRESULT CancelAndDrain();

    /**
     * @brief Waits for completion while polling SAPI's real-time abort action.
     * @param pOutputSite The output site from the current ISpTTSEngine::Speak call, or null during teardown.
     * @return S_OK when the request completes or is cleanly cancelled; an HRESULT on cancellation failure.
     */
    HRESULT WaitUntilFinished(ISpTTSEngineSite* pOutputSite);

    /**
     * @brief Reports whether cancellation transport failed and this worker has quarantined its pipe session.
     */
    bool IsFaulted() const;

private:
    /**
     * @brief Thread procedure for reading PCM audio bytes and dispatching to engine.
     */
    void AudioThreadProc();

    /**
     * @brief Thread procedure for reading JSON events and dispatching to engine.
     */
    void ControlThreadProc();

    /**
     * @brief Completes speech only when the provider-declared PCM boundary has reached SAPI.
     */
    void CompleteIfAudioBoundaryReached();

    /**
     * @brief Completes cancellation only after all provider-committed old PCM has been discarded.
     */
    void CompleteCancellationIfAudioBoundaryReached();

    /**
     * @brief Sends a cancellation command for a specific request without changing worker state.
     */
    HRESULT SendCancellation(uint64_t speakId);

    /**
     * @brief Records a terminal cancellation transport failure without touching pipe or thread ownership.
     */
    void EnterFaultedState();

    enum class RequestState
    {
        Idle,
        Speaking,
        Cancelling,
        Faulted
    };

    CSapiEngine* m_pEngine;                  /**< Pointer to parent SAPI engine. */
    std::shared_ptr<PipeClient> m_pClient;   /**< Shared IPC PipeClient. */

    std::thread m_audioThread;               /**< Audio streaming worker thread. */
    std::thread m_controlThread;             /**< Control event worker thread. */
    std::atomic_bool m_exit;                 /**< Flag indicating worker shutdown. */
    mutable std::mutex m_requestMutex;       /**< Serializes request lifecycle state across audio and control threads. */
    std::condition_variable m_requestChanged;/**< Wakes synchronous Speak and purge callers at terminal boundaries. */
    RequestState m_requestState{RequestState::Idle}; /**< Active request lifecycle state. */
    uint64_t m_activeSpeakId{0};             /**< Identifier for the active request. */
    bool m_synthesisComplete{false};         /**< Provider has declared the final PCM byte count. */
    bool m_cancellationComplete{false};      /**< Provider has declared the final cancellation byte count. */
    bool m_cancellationFailed{false};        /**< Cancellation could not reach a valid terminal boundary. */
    uint64_t m_expectedAudioBytes{0};        /**< Declared raw PCM byte count for normal completion. */
    uint64_t m_cancelledAudioBytes{0};       /**< Raw PCM bytes committed before cancellation. */
    uint64_t m_rawAudioBytesRead{0};         /**< Raw bytes consumed from the audio pipe for the active request. */
    uint64_t m_deliveredAudioBytes{0};       /**< Bytes successfully written to ISpTTSEngineSite. */
};
