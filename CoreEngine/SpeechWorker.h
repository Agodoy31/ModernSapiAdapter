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
#include "PcmFrameAssembler.h"
#include "SpeechWorkerTypes.h"

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
     * @param pClient Raw PipeClient IPC transport owned by CSapiEngine.
     */
    SpeechWorker(CSapiEngine* pEngine, PipeClient* pClient, WORD blockAlign);

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

#if defined(_DEBUG)
    void PauseNextEventForwardForTest();
    bool WaitForEventForwardPauseForTest(DWORD timeoutMs);
    void ReleaseEventForwardForTest();
    void PauseNextAbortTransitionForTest();
    bool WaitForAbortTransitionPauseForTest(DWORD timeoutMs);
    void ReleaseAbortTransitionForTest();
    bool WasCancellingAtAbortUnlockForTest();
    bool WaitForFaultForTest(DWORD timeoutMs);
    uint64_t RawAudioBytesForTest() const;
    void PauseNextFaultPublicationForTest();
    bool WaitForFaultPublicationPauseForTest(DWORD timeoutMs);
    void ReleaseFaultPublicationForTest();
    void FailNextFrameAssemblyForTest();
    bool IsAudioApartmentActiveForTest() const noexcept;
    static void FailNextControlThreadCreationForTest();
    static void FailNextControlThreadEntryForTest();
#endif

private:
    friend class CSapiEngine;

    static constexpr ULONGLONG SynthesisInactivityTimeoutMs = 1500;
    static constexpr DWORD CancellationTimeoutMs = 500;

    /**
     * @brief Thread procedure for reading PCM audio bytes and dispatching to engine.
     */
    void AudioThreadProc();

    /**
     * @brief Thread procedure for reading JSON events and dispatching to engine.
     */
    void ControlThreadProc();

    /**
     * @brief Checks if the terminal boundary has been reached for the upstream and downstream states.
     */
    bool CheckTerminalBoundaryLocked();

    [[nodiscard]] bool HasSpeakingAudioOverrunLocked() const noexcept;
    [[nodiscard]] bool IsSpeakingTerminalReachedLocked() const noexcept;
    [[nodiscard]] bool IsCancellingTerminalReachedLocked() const noexcept;
    void ResetToIdleLocked() noexcept;

    /**
     * @brief Publishes the active request's cancellation state while m_requestMutex is held.
     */
    HRESULT BeginCancellationLocked(ULONGLONG cancellationDeadline,
                                    ULONGLONG cancellationEntryTick,
                                    uint64_t& speakId);

    /**
     * @brief Sends cancellation and waits for the exact provider-declared PCM boundary.
     */
    HRESULT FinishCancellation(uint64_t speakId,
                               ULONGLONG cancellationDeadline,
                               ULONGLONG cancellationEntryTick);

    /**
     * @brief Sends a cancellation command for a specific request without changing worker state.
     */
    HRESULT SendCancellation(uint64_t speakId, DWORD timeoutMs);

    /**
     * @brief Quarantines the session and cancels outstanding pipe I/O without changing pipe ownership.
     */
    void EnterFaultedState();

    void ForwardEventToSapi(const nlohmann::json& json);

    CSapiEngine* m_pEngine;                  /**< Pointer to parent SAPI engine. */
    PipeClient* m_pClient;                   /**< Engine-owned IPC PipeClient. */

    std::thread m_audioThread;               /**< Audio streaming worker thread. */
    std::thread m_controlThread;             /**< Control event worker thread. */
    std::atomic_bool m_exit;                 /**< Flag indicating worker shutdown. */
    std::atomic<ULONGLONG> m_lastProviderProgressTick{0}; /**< Monotonic tick of active-request PCM or control progress. */
    mutable std::mutex m_requestMutex;       /**< Serializes request lifecycle state across audio and control threads. */
    std::recursive_mutex m_eventForwardMutex;/**< Serializes event callbacks with Faulted publication; recursive for COM re-entrancy. */
    std::atomic_bool m_faultPublicationStarted{false}; /**< Ensures concurrent pipe failures publish quarantine once. */
    std::atomic_bool m_faultVisible{false};  /**< Prevents new SAPI event callbacks once a fault is visible. */
    std::condition_variable m_requestChanged;/**< Wakes synchronous Speak and purge callers at terminal boundaries. */
    UpstreamState m_upstreamState{UpstreamState::Idle}; /**< Active request upstream lifecycle state. */
    DownstreamState m_downstreamState{DownstreamState::Idle}; /**< Active request downstream lifecycle state. */
    bool m_faultPending{false};              /**< A protocol violation was detected before full Faulted publication. */
    uint64_t m_activeSpeakId{0};             /**< Identifier for the active request. */
    uint64_t m_upstreamTerminalBytes{0};     /**< Declared raw PCM byte count for normal completion or cancellation. */
    bool m_upstreamFinished{false};          /**< Provider has declared the final PCM byte count. */
    HRESULT m_requestCompletionHr{S_OK};     /**< Result of the active request. */
    ULONGLONG m_cancellationDeadlineTick{0}; /**< Absolute deadline for the active cancellation transaction. */
    uint64_t m_rawAudioBytesRead{0};         /**< Raw bytes consumed from the audio pipe for the active request. */
    uint64_t m_deliveredAudioBytes{0};       /**< Bytes successfully written to ISpTTSEngineSite. */
    PcmFrameAssembler m_frameAssembler;      /**< Reassembles provider-native PCM frames under m_requestMutex. */
#if defined(_DEBUG)
    std::mutex m_eventForwardTestMutex;
    std::condition_variable m_eventForwardTestChanged;
    bool m_pauseNextEventForwardForTest{false};
    bool m_eventForwardPausedForTest{false};
    std::mutex m_abortTransitionTestMutex;
    std::condition_variable m_abortTransitionTestChanged;
    bool m_pauseNextAbortTransitionForTest{false};
    bool m_abortTransitionPausedForTest{false};
    bool m_wasCancellingAtAbortUnlockForTest{false};
    std::mutex m_faultPublicationTestMutex;
    std::condition_variable m_faultPublicationTestChanged;
    bool m_pauseNextFaultPublicationForTest{false};
    bool m_faultPublicationPausedForTest{false};
    bool m_failNextFrameAssemblyForTest{false};
    std::atomic_bool m_audioApartmentActiveForTest{false};
#endif
};
