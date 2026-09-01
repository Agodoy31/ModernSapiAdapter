#include "pch.h"
#include "SpeechWorker.h"
#include "SpeechWorkerTypes.h"
#include "SpeechProtocolUtils.h"
#include "SapiEngine.h"
#include "JsonValue.h"

using namespace SpeechProtocolUtils;

namespace
{
#if defined(_DEBUG)
std::atomic_bool g_failNextControlThreadCreationForTest{false};
std::atomic_bool g_failNextControlThreadEntryForTest{false};
#endif
}

SpeechWorker::SpeechWorker(CSapiEngine* pEngine, PipeClient* pClient, WORD blockAlign)
    : m_pEngine(pEngine), m_pClient(pClient), m_exit(false), m_frameAssembler(blockAlign)
{
    m_audioThread = std::thread([this] {
        try
        {
            AudioThreadProc();
        }
        catch (const std::exception& error)
        {
            CoreLog(L"[SpeechWorker] Unhandled audio worker exception: %hs", error.what());
            EnterFaultedState();
        }
        catch (...)
        {
            CoreLog(L"[SpeechWorker] Unhandled unknown audio worker exception.");
            EnterFaultedState();
        }
    });
    try
    {
#if defined(_DEBUG)
        if (g_failNextControlThreadCreationForTest.exchange(false))
        {
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        }
#endif
        m_controlThread = std::thread([this] {
            try
            {
#if defined(_DEBUG)
                if (g_failNextControlThreadEntryForTest.exchange(false))
                {
                    throw std::runtime_error("Injected control worker entry failure");
                }
#endif
                ControlThreadProc();
            }
            catch (const std::exception& error)
            {
                CoreLog(L"[SpeechWorker] Unhandled control worker exception: %hs", error.what());
                EnterFaultedState();
            }
            catch (...)
            {
                CoreLog(L"[SpeechWorker] Unhandled unknown control worker exception.");
                EnterFaultedState();
            }
        });
    }
    catch (...)
    {
        m_exit.store(true);
        if (m_pClient)
        {
            m_pClient->Cancel();
        }
        if (m_audioThread.joinable())
        {
            m_audioThread.join();
        }
        throw;
    }
}

SpeechWorker::~SpeechWorker()
{
    m_exit.store(true);
#if defined(_DEBUG)
    ReleaseEventForwardForTest();
    ReleaseAbortTransitionForTest();
    ReleaseFaultPublicationForTest();
#endif
    
    // Cancel I/O so blocking ReadFile/GetOverlappedResult returns immediately
    if (m_pClient)
    {
        m_pClient->Cancel();
    }

    if (m_audioThread.joinable()) m_audioThread.join();
    if (m_controlThread.joinable()) m_controlThread.join();
}

bool SpeechWorker::Start(uint64_t speakId)
{
    std::lock_guard<std::mutex> lock(m_requestMutex);
    if (m_context.upstreamState != UpstreamState::Idle ||
        m_context.downstreamState != DownstreamState::Idle ||
        m_context.faultPending)
    {
        return false;
    }

    m_context.Reset();
    m_context.token.speakId = speakId;
    m_context.token.generation = ++m_generationCounter;
    m_frameAssembler.Reset();
    m_lastProviderProgressTick.store(GetTickCount64(), std::memory_order_release);
    m_context.upstreamState = UpstreamState::Active;
    m_context.downstreamState = DownstreamState::Speaking;
    return true;
}

bool SpeechWorker::IsFaulted() const
{
    std::lock_guard<std::mutex> lock(m_requestMutex);
    return m_context.downstreamState == DownstreamState::Faulted ||
           m_context.upstreamState == UpstreamState::Faulted ||
           m_context.faultPending;
}

#if defined(_DEBUG)
void SpeechWorker::FailNextControlThreadCreationForTest()
{
    g_failNextControlThreadCreationForTest.store(true);
}

void SpeechWorker::FailNextControlThreadEntryForTest()
{
    g_failNextControlThreadEntryForTest.store(true);
}

void SpeechWorker::PauseNextEventForwardForTest()
{
    std::lock_guard<std::mutex> lock(m_testHooks.eventForwardMutex);
    m_testHooks.pauseNextEventForward = true;
    m_testHooks.eventForwardPaused = false;
}

bool SpeechWorker::WaitForEventForwardPauseForTest(DWORD timeoutMs)
{
    std::unique_lock<std::mutex> lock(m_testHooks.eventForwardMutex);
    return m_testHooks.eventForwardChanged.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
        return m_testHooks.eventForwardPaused || m_exit.load();
    }) && m_testHooks.eventForwardPaused;
}

void SpeechWorker::ReleaseEventForwardForTest()
{
    std::lock_guard<std::mutex> lock(m_testHooks.eventForwardMutex);
    m_testHooks.pauseNextEventForward = false;
    m_testHooks.eventForwardPaused = false;
    m_testHooks.eventForwardChanged.notify_all();
}

void SpeechWorker::PauseNextAbortTransitionForTest()
{
    std::lock_guard<std::mutex> lock(m_testHooks.abortTransitionMutex);
    m_testHooks.pauseNextAbortTransition = true;
    m_testHooks.abortTransitionPaused = false;
    m_testHooks.wasCancellingAtAbortUnlock = false;
}

bool SpeechWorker::WaitForAbortTransitionPauseForTest(DWORD timeoutMs)
{
    std::unique_lock<std::mutex> lock(m_testHooks.abortTransitionMutex);
    return m_testHooks.abortTransitionChanged.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
        return m_testHooks.abortTransitionPaused || m_exit.load();
    }) && m_testHooks.abortTransitionPaused;
}

void SpeechWorker::ReleaseAbortTransitionForTest()
{
    std::lock_guard<std::mutex> lock(m_testHooks.abortTransitionMutex);
    m_testHooks.pauseNextAbortTransition = false;
    m_testHooks.abortTransitionPaused = false;
    m_testHooks.abortTransitionChanged.notify_all();
}

bool SpeechWorker::WasCancellingAtAbortUnlockForTest()
{
    std::lock_guard<std::mutex> lock(m_testHooks.abortTransitionMutex);
    return m_testHooks.wasCancellingAtAbortUnlock;
}

bool SpeechWorker::WaitForFaultForTest(DWORD timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!m_faultVisible.load(std::memory_order_acquire) && !m_exit.load())
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        Sleep(1);
    }
    return m_faultVisible.load(std::memory_order_acquire);
}

uint64_t SpeechWorker::RawAudioBytesForTest() const
{
    std::lock_guard<std::mutex> lock(m_requestMutex);
    return m_context.rawAudioBytesRead;
}

void SpeechWorker::PauseNextFaultPublicationForTest()
{
    std::lock_guard<std::mutex> lock(m_testHooks.faultPublicationMutex);
    m_testHooks.pauseNextFaultPublication = true;
    m_testHooks.faultPublicationPaused = false;
}

bool SpeechWorker::WaitForFaultPublicationPauseForTest(DWORD timeoutMs)
{
    std::unique_lock<std::mutex> lock(m_testHooks.faultPublicationMutex);
    return m_testHooks.faultPublicationChanged.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
        return m_testHooks.faultPublicationPaused || m_exit.load();
    }) && m_testHooks.faultPublicationPaused;
}

void SpeechWorker::ReleaseFaultPublicationForTest()
{
    std::lock_guard<std::mutex> lock(m_testHooks.faultPublicationMutex);
    m_testHooks.pauseNextFaultPublication = false;
    m_testHooks.faultPublicationPaused = false;
    m_testHooks.faultPublicationChanged.notify_all();
}

void SpeechWorker::FailNextFrameAssemblyForTest()
{
    std::lock_guard<std::mutex> lock(m_requestMutex);
    m_testHooks.failNextFrameAssembly = true;
}

bool SpeechWorker::IsAudioApartmentActiveForTest() const noexcept
{
    return m_testHooks.audioApartmentActive.load(std::memory_order_acquire);
}
#endif

void SpeechWorker::Stop()
{
    uint64_t speakId = 0;
    {
        std::lock_guard<std::mutex> lock(m_requestMutex);
        if (m_context.downstreamState == DownstreamState::Idle)
        {
            return;
        }

        if (m_context.downstreamState == DownstreamState::Faulted ||
            m_context.upstreamState == UpstreamState::Faulted)
        {
            return;
        }

        speakId = m_context.token.speakId;
        m_frameAssembler.Reset();
        m_context.upstreamState = UpstreamState::Idle;
        m_context.downstreamState = DownstreamState::Idle;
        m_requestChanged.notify_all();
    }

    SendCancellation(speakId, CancellationTimeoutMs);
}

HRESULT SpeechWorker::CancelAndDrain()
{
    const ULONGLONG cancellationEntryTick = GetTickCount64();
    const ULONGLONG cancellationDeadline = cancellationEntryTick + CancellationTimeoutMs;
#if defined(_DEBUG)
    CoreLog(L"[CancelTrace] cancel_and_drain_enter tick=%llu.", cancellationEntryTick);
#endif
    uint64_t speakId = 0;
    {
        std::lock_guard<std::mutex> lock(m_requestMutex);
        const HRESULT transitionHr = BeginCancellationLocked(
            cancellationDeadline, cancellationEntryTick, speakId);
        if (transitionHr == S_FALSE)
        {
            return S_OK;
        }
        if (FAILED(transitionHr))
        {
            return transitionHr;
        }
    }

    return FinishCancellation(speakId, cancellationDeadline, cancellationEntryTick);
}

HRESULT SpeechWorker::BeginCancellationLocked(ULONGLONG cancellationDeadline,
                                              ULONGLONG cancellationEntryTick,
                                              uint64_t& speakId)
{
    if (m_context.downstreamState == DownstreamState::Idle)
    {
        return S_FALSE;
    }

    if (m_context.downstreamState == DownstreamState::Faulted ||
        m_context.upstreamState == UpstreamState::Faulted)
    {
        return E_FAIL;
    }

    if (m_context.IsDrainingCancellation())
    {
        return E_UNEXPECTED;
    }

    speakId = m_context.token.speakId;
    m_context.TransitionToCancelling(cancellationDeadline);
    m_frameAssembler.Reset();
#if defined(_DEBUG)
    CoreLog(L"[CancelTrace] speak_id=%llu cancelling_published tick=%llu entry_to_publish_ms=%llu raw=%llu delivered=%llu.",
        speakId, GetTickCount64(), GetTickCount64() - cancellationEntryTick,
        m_context.rawAudioBytesRead, m_context.deliveredAudioBytes);
#endif
    return S_OK;
}

HRESULT SpeechWorker::FinishCancellation(uint64_t speakId,
                                         ULONGLONG cancellationDeadline,
                                         ULONGLONG cancellationEntryTick)
{
    const ULONGLONG beforeSend = GetTickCount64();
    const DWORD sendTimeout = beforeSend < cancellationDeadline
        ? static_cast<DWORD>(cancellationDeadline - beforeSend)
        : 0;
#if defined(_DEBUG)
    CoreLog(L"[CancelTrace] speak_id=%llu cancel_send_begin tick=%llu remaining_budget_ms=%lu.",
        speakId, beforeSend, sendTimeout);
#endif
    HRESULT hr = SendCancellation(speakId, sendTimeout);
#if defined(_DEBUG)
    CoreLog(L"[CancelTrace] speak_id=%llu cancel_send_end tick=%llu duration_ms=%llu hr=0x%08x.",
        speakId, GetTickCount64(), GetTickCount64() - beforeSend, hr);
#endif
    if (FAILED(hr))
    {
        CoreLog(L"[SpeechWorker] Cancellation command failed for speak_id %llu: 0x%08x.", speakId, hr);
        EnterFaultedState();
        return hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT) ? hr : E_FAIL;
    }

    std::unique_lock<std::mutex> lock(m_requestMutex);
    const ULONGLONG beforeWait = GetTickCount64();
    const auto remaining = std::chrono::milliseconds(beforeWait < cancellationDeadline
        ? cancellationDeadline - beforeWait
        : 0);
    const bool completed = m_requestChanged.wait_for(lock, remaining, [this] {
        return !m_context.IsDrainingCancellation() || m_exit.load();
    });

    if (!completed && m_context.IsDrainingCancellation())
    {
#if defined(_DEBUG)
        const uint64_t rawAudioBytes = m_context.rawAudioBytesRead;
        const uint64_t cancelledAudioBytes = m_context.upstreamTerminalBytes;
        const bool terminalReceived = m_context.upstreamFinished;
#endif
        lock.unlock();
        CoreLog(L"[SpeechWorker] Provider did not complete cancellation within %lu ms for speak_id %llu; quarantining session.",
            CancellationTimeoutMs, speakId);
#if defined(_DEBUG)
        CoreLog(L"[CancelTrace] speak_id=%llu cancellation_timeout tick=%llu elapsed_ms=%llu terminal_received=%u declared=%llu raw=%llu.",
            speakId, GetTickCount64(), GetTickCount64() - cancellationEntryTick,
            terminalReceived ? 1u : 0u, cancelledAudioBytes, rawAudioBytes);
#endif
        EnterFaultedState();
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }

#if defined(_DEBUG)
    CoreLog(L"[CancelTrace] speak_id=%llu cancel_and_drain_end tick=%llu elapsed_ms=%llu state=%u terminal_received=%u declared=%llu raw=%llu.",
        speakId, GetTickCount64(), GetTickCount64() - cancellationEntryTick,
        static_cast<unsigned>(m_context.downstreamState), m_context.upstreamFinished ? 1u : 0u,
        m_context.upstreamTerminalBytes, m_context.rawAudioBytesRead);
#endif
    return m_exit.load() ? E_ABORT : m_context.completionHr;
}

void SpeechWorker::EnterFaultedState()
{
    {
        std::lock_guard<std::mutex> requestLock(m_requestMutex);
        m_context.completionHr = E_FAIL;
        m_frameAssembler.Reset();
        m_context.upstreamState = UpstreamState::Faulted;
        m_context.downstreamState = DownstreamState::Faulted;
        m_requestChanged.notify_all();

        bool expected = false;
        if (!m_faultPublicationStarted.compare_exchange_strong(expected, true))
        {
            return;
        }
        m_context.faultPending = true;
    }

#if defined(_DEBUG)
    {
        std::unique_lock<std::mutex> testLock(m_testHooks.faultPublicationMutex);
        if (m_testHooks.pauseNextFaultPublication)
        {
            m_testHooks.pauseNextFaultPublication = false;
            m_testHooks.faultPublicationPaused = true;
            m_testHooks.faultPublicationChanged.notify_all();
            m_testHooks.faultPublicationChanged.wait(testLock, [this] {
                return !m_testHooks.faultPublicationPaused || m_exit.load();
            });
        }
    }
#endif

    {
        std::lock_guard<std::recursive_mutex> eventForwardLock(m_eventForwardMutex);
        m_faultVisible.store(true, std::memory_order_release);

        std::lock_guard<std::mutex> requestLock(m_requestMutex);
        m_context.faultPending = false;
    }

    if (m_pClient)
    {
        m_pClient->Cancel();
    }
}

void SpeechWorker::ForwardEventToSapi(const nlohmann::json& json)
{
    std::lock_guard<std::recursive_mutex> eventForwardLock(m_eventForwardMutex);
    if (m_faultVisible.load(std::memory_order_acquire))
    {
        return;
    }

    uint64_t eventSpeakId = 0;
    if (!json.contains("speak_id") ||
        !TryGetJsonUnsignedInteger(json["speak_id"], eventSpeakId))
    {
        return;
    }

    const bool isLog = json.contains("event") && json["event"].is_string() && json["event"].get<std::string_view>() == "log";

    {
        std::lock_guard<std::mutex> requestLock(m_requestMutex);
        if (!ShouldForwardEventLocked(eventSpeakId, isLog))
        {
            return;
        }
    }

#if defined(_DEBUG)
    const ULONGLONG callbackStartTick = GetTickCount64();
    const std::string_view eventName = json.contains("event") && json["event"].is_string()
        ? json["event"].get<std::string_view>()
        : std::string_view("invalid");
    CoreLog(L"[CancelTrace] speak_id=%llu sapi_event_begin event=%hs tick=%llu.",
        eventSpeakId, eventName.data(), callbackStartTick);
#endif
    m_pEngine->OnSpeechEvent(json);
#if defined(_DEBUG)
    const ULONGLONG callbackEndTick = GetTickCount64();
    DownstreamState stateAfterCallback = DownstreamState::Faulted;
    {
        std::lock_guard<std::mutex> requestLock(m_requestMutex);
        stateAfterCallback = m_context.downstreamState;
    }
    const ULONGLONG callbackDuration = callbackEndTick - callbackStartTick;
    if (callbackDuration >= 5 || (stateAfterCallback != DownstreamState::Speaking && !isLog))
    {
        CoreLog(L"[CancelTrace] speak_id=%llu sapi_event_end event=%hs tick=%llu duration_ms=%llu state_after=%u.",
            eventSpeakId, eventName.data(), callbackEndTick, callbackDuration,
            static_cast<unsigned>(stateAfterCallback));
    }
#endif
}

HRESULT SpeechWorker::SendCancellation(uint64_t speakId, DWORD timeoutMs)
{
    try
    {
        nlohmann::json req = {
            {"command", "cancel"},
            {"speak_id", speakId}
        };
        return m_pClient ? m_pClient->SendControlMessage(req, timeoutMs) : E_UNEXPECTED;
    }
    catch (const std::exception& e)
    {
        CoreLog(L"[SpeechWorker] Exception while cancelling speak_id %llu: %hs", speakId, e.what());
        return E_FAIL;
    }
    catch (...)
    {
        CoreLog(L"[SpeechWorker] Unknown exception while cancelling speak_id %llu.", speakId);
        return E_FAIL;
    }
}

HRESULT SpeechWorker::WaitUntilFinished(ISpTTSEngineSite* pOutputSite)
{
    std::unique_lock<std::mutex> lock(m_requestMutex);
#if defined(_DEBUG)
    ULONGLONG lastTerminalWaitLogTick = 0;
#endif
    while (m_context.downstreamState != DownstreamState::Idle &&
           m_context.downstreamState != DownstreamState::Faulted &&
           !m_exit.load())
    {
        if (m_requestChanged.wait_for(lock, std::chrono::milliseconds(10), [this] {
            return m_context.downstreamState == DownstreamState::Idle ||
                   m_context.downstreamState == DownstreamState::Faulted ||
                   m_exit.load();
        }))
        {
            break;
        }

        const ULONGLONG now = GetTickCount64();
#if defined(_DEBUG)
        if (m_context.IsAwaitingTerminalAudio() &&
            (lastTerminalWaitLogTick == 0 || now - lastTerminalWaitLogTick >= 250))
        {
            CoreLog(L"[ThreadTrace] speak_id=%llu terminal_pending tick=%llu declared=%llu raw=%llu delivered=%llu carry=%u.",
                m_context.token.speakId, now, m_context.upstreamTerminalBytes, m_context.rawAudioBytesRead,
                m_context.deliveredAudioBytes, m_frameAssembler.HasCarry() ? 1u : 0u);
            lastTerminalWaitLogTick = now;
        }
#endif
        if (m_context.IsDrainingCancellation() &&
            HasCancellationTimedOut(now, m_context.cancellationDeadlineTick))
        {
            const uint64_t speakId = m_context.token.speakId;
            lock.unlock();
            CoreLog(L"[SpeechWorker] Provider did not complete cancellation within %lu ms for speak_id %llu; quarantining session.",
                CancellationTimeoutMs, speakId);
            EnterFaultedState();
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }

        const ULONGLONG lastProgress = m_lastProviderProgressTick.load(std::memory_order_acquire);
        const bool isActivelySynthesizing = m_context.IsActivelySynthesizing();
        const bool isAwaitingTerminalAudio = m_context.IsAwaitingTerminalAudio();

        if ((isActivelySynthesizing || isAwaitingTerminalAudio) &&
            HasSynthesisInactivityTimedOut(now, lastProgress, SynthesisInactivityTimeoutMs))
        {
            const uint64_t speakId = m_context.token.speakId;
            lock.unlock();
            CoreLog(L"[SpeechWorker] Provider made no progress for %llu ms during speak_id %llu (awaiting_terminal=%d); quarantining session.",
                SynthesisInactivityTimeoutMs, speakId, isAwaitingTerminalAudio ? 1 : 0);
            EnterFaultedState();
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }

        if (!pOutputSite)
        {
            continue;
        }

        lock.unlock();
        const DWORD actions = pOutputSite->GetActions();
        lock.lock();

        if (IsAbortRequested(actions) && m_context.downstreamState == DownstreamState::Speaking)
        {
            const ULONGLONG cancellationEntryTick = GetTickCount64();
            const ULONGLONG cancellationDeadline = cancellationEntryTick + CancellationTimeoutMs;
#if defined(_DEBUG)
            CoreLog(L"[CancelTrace] speak_id=%llu sapi_abort_observed tick=%llu state=%u raw=%llu delivered=%llu.",
                m_context.token.speakId, cancellationEntryTick, static_cast<unsigned>(m_context.downstreamState),
                m_context.rawAudioBytesRead, m_context.deliveredAudioBytes);
#endif
            uint64_t speakId = 0;
            const HRESULT transitionHr = BeginCancellationLocked(
                cancellationDeadline, cancellationEntryTick, speakId);
            if (FAILED(transitionHr))
            {
                return transitionHr;
            }
#if defined(_DEBUG)
            {
                std::lock_guard<std::mutex> testLock(m_testHooks.abortTransitionMutex);
                m_testHooks.wasCancellingAtAbortUnlock = m_context.IsDrainingCancellation();
            }
#endif
            lock.unlock();
#if defined(_DEBUG)
            {
                std::unique_lock<std::mutex> testLock(m_testHooks.abortTransitionMutex);
                if (m_testHooks.pauseNextAbortTransition)
                {
                    m_testHooks.pauseNextAbortTransition = false;
                    m_testHooks.abortTransitionPaused = true;
                    m_testHooks.abortTransitionChanged.notify_all();
                    m_testHooks.abortTransitionChanged.wait(testLock, [this] {
                        return !m_testHooks.abortTransitionPaused || m_exit.load();
                    });
                }
            }
#endif
            CoreLog(L"[SpeechWorker] SAPI requested SPVES_ABORT; cancelling active synthesis.");
            return FinishCancellation(speakId, cancellationDeadline, cancellationEntryTick);
        }
    }

    if (m_exit.load())
    {
        return E_ABORT;
    }

    return m_context.completionHr;
}

bool SpeechWorker::HasSpeakingAudioOverrunLocked() const noexcept
{
    return m_context.rawAudioBytesRead > m_context.upstreamTerminalBytes ||
           m_context.deliveredAudioBytes > m_context.upstreamTerminalBytes;
}

bool SpeechWorker::IsSpeakingTerminalReachedLocked() const noexcept
{
    return m_context.rawAudioBytesRead == m_context.upstreamTerminalBytes &&
           m_context.deliveredAudioBytes == m_context.upstreamTerminalBytes &&
           !m_frameAssembler.HasCarry();
}

bool SpeechWorker::IsCancellingTerminalReachedLocked() const noexcept
{
    return m_context.rawAudioBytesRead >= m_context.upstreamTerminalBytes;
}

bool SpeechWorker::ShouldForwardEventLocked(uint64_t speakId, bool isLog) const noexcept
{
    if (speakId != m_context.token.speakId)
    {
        return false;
    }

    if (isLog)
    {
        return true;
    }

    if (m_context.faultPending)
    {
        return false;
    }

    return m_context.downstreamState == DownstreamState::Speaking;
}

void SpeechWorker::ResetToIdleLocked() noexcept
{
    m_frameAssembler.Reset();
    m_context.upstreamState = UpstreamState::Idle;
    m_context.downstreamState = DownstreamState::Idle;
    m_requestChanged.notify_all();
}

bool SpeechWorker::CheckTerminalBoundaryLocked()
{
    if (m_context.upstreamState == UpstreamState::Failed)
    {
        ResetToIdleLocked();
        return false;
    }

    if (!m_context.upstreamFinished)
    {
        return false;
    }

    switch (m_context.downstreamState)
    {
    case DownstreamState::Speaking:
    {
        if (HasSpeakingAudioOverrunLocked())
        {
            CoreLog(L"[SpeechWorker] Provider audio overrun: raw=%llu delivered=%llu declared=%llu",
                m_context.rawAudioBytesRead, m_context.deliveredAudioBytes, m_context.upstreamTerminalBytes);
            return true;
        }
        else if (IsSpeakingTerminalReachedLocked())
        {
#if defined(_DEBUG)
            CoreLog(L"[ThreadTrace] speak_id=%llu terminal_boundary_reached tick=%llu declared=%llu raw=%llu delivered=%llu.",
                m_context.token.speakId, GetTickCount64(), m_context.upstreamTerminalBytes,
                m_context.rawAudioBytesRead, m_context.deliveredAudioBytes);
#endif
            ResetToIdleLocked();
            return false;
        }
        else
        {
            return false;
        }
    }

    case DownstreamState::Cancelling:
    {
        if (IsCancellingTerminalReachedLocked())
        {
#if defined(_DEBUG)
            const ULONGLONG cancellationStartTick = m_context.cancellationDeadlineTick >= CancellationTimeoutMs
                ? m_context.cancellationDeadlineTick - CancellationTimeoutMs
                : 0;
            CoreLog(L"[CancelTrace] speak_id=%llu cancellation_boundary_reached tick=%llu elapsed_ms=%llu declared=%llu raw=%llu.",
                m_context.token.speakId, GetTickCount64(),
                cancellationStartTick == 0 ? 0 : GetTickCount64() - cancellationStartTick,
                m_context.upstreamTerminalBytes, m_context.rawAudioBytesRead);
#endif
            ResetToIdleLocked();
            return false;
        }
        else
        {
            return false;
        }
    }

    case DownstreamState::Idle:
    case DownstreamState::Faulted:
        return false;
    }

    return false;
}

SpeechWorker::AudioIngestResult SpeechWorker::IngestAudioChunkLocked(const uint8_t* pChunkData, DWORD bytesRead)
{
    AudioIngestResult result{};
    result.token = m_context.token;

    if (m_context.downstreamState == DownstreamState::Speaking || m_context.IsDrainingCancellation())
    {
        m_lastProviderProgressTick.store(GetTickCount64(), std::memory_order_release);
    }

    switch (m_context.downstreamState)
    {
    case DownstreamState::Speaking:
    {
        size_t bytesToFrame = bytesRead;
        if (m_context.upstreamFinished)
        {
            const uint64_t remainingDeclaredBytes = m_context.upstreamTerminalBytes > m_context.rawAudioBytesRead
                ? m_context.upstreamTerminalBytes - m_context.rawAudioBytesRead
                : 0;
            bytesToFrame = static_cast<size_t>((std::min)(static_cast<uint64_t>(bytesRead), remainingDeclaredBytes));
        }
        m_context.rawAudioBytesRead += bytesRead;
#if defined(_DEBUG)
        if (m_testHooks.failNextFrameAssembly)
        {
            m_testHooks.failNextFrameAssembly = false;
            throw std::bad_alloc();
        }
#endif
        result.spansToWrite = m_frameAssembler.Process(pChunkData, bytesToFrame);
        if (result.spansToWrite.empty())
        {
            result.protocolBoundaryFailed = CheckTerminalBoundaryLocked();
        }
        else
        {
            result.shouldDeliverAudio = true;
        }
        break;
    }

    case DownstreamState::Cancelling:
    {
#if defined(_DEBUG)
        const uint64_t rawBeforeRead = m_context.rawAudioBytesRead;
#endif
        m_context.rawAudioBytesRead += bytesRead;
#if defined(_DEBUG)
        if (m_context.upstreamFinished)
        {
            CoreLog(L"[CancelTrace] speak_id=%llu cancellation_audio_read tick=%llu chunk=%lu raw_before=%llu raw_after=%llu declared=%llu.",
                m_context.token.speakId, GetTickCount64(), bytesRead, rawBeforeRead,
                m_context.rawAudioBytesRead, m_context.upstreamTerminalBytes);
        }
#endif
        result.protocolBoundaryFailed = CheckTerminalBoundaryLocked();
        break;
    }

    case DownstreamState::Idle:
    {
        CoreLog(L"[SpeechWorker] Provider sent audio after the active request reached its terminal boundary.");
        result.protocolBoundaryFailed = true;
        break;
    }

    case DownstreamState::Faulted:
    {
        // Continue draining provider PCM so its audio pipe cannot fill, but never call SAPI.
        break;
    }
    }

    if (result.protocolBoundaryFailed)
    {
        m_context.faultPending = true;
    }

    return result;
}

bool SpeechWorker::UpdateAfterAudioDeliveryLocked(
    const RequestToken& batchToken,
    size_t deliveredBytes,
    bool writeAccepted,
    uint64_t& outCancellationToSend)
{
    outCancellationToSend = 0;

    if (!m_context.token.Matches(batchToken))
    {
        // Stale audio batch delivered from a cancelled/previous request; ignore safely without mutating new request state.
        return false;
    }

    bool protocolBoundaryFailed = false;

    switch (m_context.downstreamState)
    {
    case DownstreamState::Speaking:
    {
        m_context.deliveredAudioBytes += deliveredBytes;
        if (!writeAccepted)
        {
            CoreLog(L"[SpeechWorker] SAPI rejected an audio write; cancelling active synthesis.");
            outCancellationToSend = m_context.token.speakId;
            m_context.TransitionToCancelling(GetTickCount64() + CancellationTimeoutMs);
            m_frameAssembler.Reset();
        }
        else
        {
            protocolBoundaryFailed = CheckTerminalBoundaryLocked();
        }
        break;
    }

    case DownstreamState::Cancelling:
    {
        protocolBoundaryFailed = CheckTerminalBoundaryLocked();
        break;
    }

    case DownstreamState::Idle:
    case DownstreamState::Faulted:
    {
        break;
    }
    }

    if (protocolBoundaryFailed)
    {
        m_context.faultPending = true;
    }

    return protocolBoundaryFailed;
}

bool SpeechWorker::IsAudioDeliveryEligibleLocked(const RequestToken& token) const noexcept
{
    return m_context.token.Matches(token) &&
           m_context.downstreamState == DownstreamState::Speaking &&
           !m_context.faultPending;
}

void SpeechWorker::AudioThreadProc()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    auto apartmentCleanup = wil::scope_exit([this] {
#if defined(_DEBUG)
        m_testHooks.audioApartmentActive.store(false, std::memory_order_release);
#endif
        winrt::uninit_apartment();
    });
#if defined(_DEBUG)
    m_testHooks.audioApartmentActive.store(true, std::memory_order_release);
#endif
    std::vector<uint8_t> buffer(4096);
    while (!m_exit.load())
    {
        DWORD bytesRead = 0;
        HRESULT hr = m_pClient->ReadAudioChunk(buffer, bytesRead);
        if (FAILED(hr))
        {
            if (m_exit.load())
            {
                break;
            }
            CoreLog(L"[SpeechWorker] Audio pipe failed; quarantining provider session: 0x%08x.", hr);
            EnterFaultedState();
            break;
        }

        if (bytesRead == 0)
        {
            continue;
        }

        AudioIngestResult ingestResult{};
        {
            std::lock_guard<std::mutex> lock(m_requestMutex);
            ingestResult = IngestAudioChunkLocked(buffer.data(), bytesRead);
        }

        uint64_t cancellationToSend = 0;
        bool deliveryFaulted = false;

        if (ingestResult.shouldDeliverAudio && !ingestResult.spansToWrite.empty())
        {
            bool writeAccepted = true;
            size_t totalBytesDeliveredInBatch = 0;

            for (const auto& span : ingestResult.spansToWrite)
            {
                {
                    std::lock_guard<std::mutex> lock(m_requestMutex);
                    if (!IsAudioDeliveryEligibleLocked(ingestResult.token))
                    {
                        break;
                    }
                }

                if (!m_pEngine->OnAudioData(span.data, static_cast<uint32_t>(span.size)))
                {
                    writeAccepted = false;
                    break;
                }
                totalBytesDeliveredInBatch += span.size;
            }

            {
                std::lock_guard<std::mutex> lock(m_requestMutex);
                deliveryFaulted = UpdateAfterAudioDeliveryLocked(
                    ingestResult.token, totalBytesDeliveredInBatch, writeAccepted, cancellationToSend);
            }
        }

        if (cancellationToSend != 0)
        {
            const HRESULT cancellationHr = SendCancellation(cancellationToSend, CancellationTimeoutMs);
            if (FAILED(cancellationHr))
            {
                CoreLog(L"[SpeechWorker] Failed to cancel speak_id %llu after SAPI rejected audio: 0x%08x.",
                    cancellationToSend, cancellationHr);
                EnterFaultedState();
            }
        }

        if (ingestResult.protocolBoundaryFailed || deliveryFaulted)
        {
            EnterFaultedState();
        }
    }
}

bool SpeechWorker::HandleTerminalEventLocked(
    ProviderEventType eventType,
    uint64_t eventSpeakId,
    uint64_t terminalAudioBytes,
    bool hasValidTerminalBytes,
    std::string_view eventStr)
{
    if (m_context.upstreamFinished)
    {
        CoreLog(L"[SpeechWorker] Duplicate terminal event for speak_id %llu.", eventSpeakId);
        m_context.upstreamState = UpstreamState::Faulted;
        m_context.downstreamState = DownstreamState::Faulted;
        m_context.completionHr = E_FAIL;
        m_requestChanged.notify_all();
        return true;
    }

    if (!hasValidTerminalBytes)
    {
        CoreLog(L"[SpeechWorker] terminal event for speak_id %llu has an invalid audio bytes value.", eventSpeakId);
        m_context.upstreamState = UpstreamState::Faulted;
        m_context.downstreamState = DownstreamState::Faulted;
        m_context.completionHr = E_FAIL;
        m_requestChanged.notify_all();
        return true;
    }

    if (terminalAudioBytes % m_frameAssembler.BlockAlign() != 0)
    {
        CoreLog(L"[SpeechWorker] terminal event for speak_id %llu is not PCM-frame aligned.", eventSpeakId);
        m_context.upstreamState = UpstreamState::Faulted;
        m_context.downstreamState = DownstreamState::Faulted;
        m_context.completionHr = E_FAIL;
        m_requestChanged.notify_all();
        return true;
    }

    m_context.upstreamTerminalBytes = terminalAudioBytes;
    m_context.upstreamFinished = true;
    m_context.upstreamState = (eventType == ProviderEventType::SynthesisComplete)
        ? UpstreamState::Completed
        : UpstreamState::Cancelled;
#if defined(_DEBUG)
    CoreLog(L"[ThreadTrace] speak_id=%llu terminal_received tick=%llu event=%hs declared=%llu raw=%llu delivered=%llu carry=%u downstream=%u.",
        eventSpeakId, GetTickCount64(), eventStr.data(), m_context.upstreamTerminalBytes,
        m_context.rawAudioBytesRead, m_context.deliveredAudioBytes,
        m_frameAssembler.HasCarry() ? 1u : 0u,
        static_cast<unsigned>(m_context.downstreamState));
#endif
    return CheckTerminalBoundaryLocked();
}

bool SpeechWorker::HandleLogEventLocked(
    uint64_t eventSpeakId,
    std::string_view logSeverity,
    std::string_view logMessage)
{
    const std::string_view severity = logSeverity.empty() ? std::string_view("info") : logSeverity;
    const std::string_view message = logMessage;

    CoreLog(L"[SpeechWorker] Provider log (severity=%.*hs, speak_id=%llu): %.*hs",
        static_cast<int>(severity.size()), severity.data(),
        eventSpeakId,
        static_cast<int>(message.size()), message.data());

    if (severity == "fatal")
    {
        return true;
    }

    if (severity == "error")
    {
        m_context.upstreamState = UpstreamState::Failed;
        m_context.upstreamFinished = true;
        m_context.completionHr = E_FAIL;
        m_context.upstreamTerminalBytes = m_context.rawAudioBytesRead;
        return CheckTerminalBoundaryLocked();
    }

    return false;
}

void SpeechWorker::ControlThreadProc()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    while (!m_exit.load())
    {
        nlohmann::json json;
        HRESULT hr = m_pClient->ReadControlMessage(json);
        if (FAILED(hr))
        {
            if (m_exit.load())
            {
                break;
            }
            CoreLog(L"[SpeechWorker] Control pipe failed; quarantining provider session: 0x%08x.", hr);
            EnterFaultedState();
            break;
        }

        try
        {
            const ProviderControlEvent event = SpeechProtocolUtils::ParseControlEvent(json);
            if (event.type == ProviderEventType::Unknown && event.rawEventName.empty())
            {
                continue;
            }

            if (event.speakId == 0)
            {
                {
                    std::lock_guard<std::mutex> lock(m_requestMutex);
                    if (m_context.upstreamState != UpstreamState::Faulted && m_context.downstreamState != DownstreamState::Faulted)
                    {
                        m_context.faultPending = true;
                    }
                }
                EnterFaultedState();
                continue;
            }

            bool forwardToSapi = false;
            bool faultAfterStateUpdate = false;

            {
                std::lock_guard<std::mutex> lock(m_requestMutex);
                forwardToSapi = (m_context.upstreamState != UpstreamState::Faulted &&
                                 m_context.downstreamState != DownstreamState::Faulted &&
                                 !m_context.faultPending);

                if (event.speakId == m_context.token.speakId)
                {
                    const bool hasValidPayload = (!event.IsSpeechBoundary() || event.hasValidSpeechOffsets) &&
                                                 (!event.IsTerminal() || event.hasValidTerminalBytes);
                    if (event.IsProgress() && hasValidPayload &&
                        (m_context.downstreamState == DownstreamState::Speaking || m_context.IsDrainingCancellation()))
                    {
                        m_lastProviderProgressTick.store(GetTickCount64(), std::memory_order_release);
                    }

                    if (event.IsSpeechBoundary())
                    {
                        if (!event.hasValidSpeechOffsets)
                        {
                            forwardToSapi = false;
                            faultAfterStateUpdate = true;
                        }
                        else if (m_context.downstreamState != DownstreamState::Speaking)
                        {
                            // SAPI has aborted this request, so delayed provider callbacks must not move focus.
                            forwardToSapi = false;
                        }
                    }

                    switch (event.type)
                    {
                    case ProviderEventType::WordBoundary:
                    case ProviderEventType::SentenceBoundary:
                    case ProviderEventType::Bookmark:
                    {
                        break;
                    }

                    case ProviderEventType::SynthesisComplete:
                    case ProviderEventType::SynthesisCancelled:
                    {
                        faultAfterStateUpdate = HandleTerminalEventLocked(
                            event.type, event.speakId, event.terminalAudioBytes, event.hasValidTerminalBytes, event.rawEventName);
                        break;
                    }

                    case ProviderEventType::LegacyCompleted:
                    {
                        CoreLog(L"[SpeechWorker] Ignoring legacy completed event for speak_id %llu.", event.speakId);
                        break;
                    }

                    case ProviderEventType::Log:
                    {
                        faultAfterStateUpdate = HandleLogEventLocked(event.speakId, event.logSeverity, event.logMessage);
                        break;
                    }

                    case ProviderEventType::Unknown:
                    {
                        CoreLog(L"[SpeechWorker] Unknown event received: %.*hs",
                            static_cast<int>(event.rawEventName.size()), event.rawEventName.data());
                        break;
                    }
                    }

                    if (faultAfterStateUpdate)
                    {
                        m_context.faultPending = true;
                    }
                }
            }

            if (forwardToSapi)
            {
#if defined(_DEBUG)
                {
                    std::unique_lock<std::mutex> testLock(m_testHooks.eventForwardMutex);
                    if (m_testHooks.pauseNextEventForward)
                    {
                        m_testHooks.pauseNextEventForward = false;
                        m_testHooks.eventForwardPaused = true;
                        m_testHooks.eventForwardChanged.notify_all();
                        m_testHooks.eventForwardChanged.wait(testLock, [this] {
                            return !m_testHooks.eventForwardPaused || m_exit.load();
                        });
                    }
                }
#endif
                ForwardEventToSapi(json);
            }

            if (faultAfterStateUpdate)
            {
                EnterFaultedState();
            }
        }
        catch (const nlohmann::json::exception& e)
        {
            CoreLog(L"[SpeechWorker] JSON exception in ControlThreadProc: %hs", e.what());
        }
        catch (const winrt::hresult_error& e)
        {
            CoreLog(L"[SpeechWorker] WinRT exception in ControlThreadProc: 0x%08x - %ls", e.code().value, e.message().c_str());
        }
        catch (const std::exception& e)
        {
            CoreLog(L"[SpeechWorker] Exception in ControlThreadProc: %hs", e.what());
        }
        catch (...)
        {
            CoreLog(L"[SpeechWorker] Unknown exception in ControlThreadProc.");
        }
    }
    winrt::uninit_apartment();
}
