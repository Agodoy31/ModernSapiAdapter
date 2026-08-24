#include "pch.h"
#include "SpeechWorker.h"
#include "SapiEngine.h"
#include "JsonValue.h"

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

bool SpeechWorker::Start(void* /*pSite*/, uint64_t speakId)
{
    std::lock_guard<std::mutex> lock(m_requestMutex);
    if (m_upstreamState != UpstreamState::Idle || m_downstreamState != DownstreamState::Idle || m_faultPending)
    {
        return false;
    }

    m_activeSpeakId = speakId;
    m_deliveredAudioBytes = 0;
    m_rawAudioBytesRead = 0;
    m_upstreamTerminalBytes = 0;
    m_upstreamFinished = false;
    m_requestCompletionHr = S_OK;
    m_cancellationDeadlineTick = 0;
    m_frameAssembler.Reset();
    m_lastProviderProgressTick.store(GetTickCount64(), std::memory_order_release);
    m_upstreamState = UpstreamState::Active;
    m_downstreamState = DownstreamState::Speaking;
    return true;
}

bool SpeechWorker::IsFaulted() const
{
    std::lock_guard<std::mutex> lock(m_requestMutex);
    return m_downstreamState == DownstreamState::Faulted || m_upstreamState == UpstreamState::Faulted || m_faultPending;
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
    std::lock_guard<std::mutex> lock(m_eventForwardTestMutex);
    m_pauseNextEventForwardForTest = true;
    m_eventForwardPausedForTest = false;
}

bool SpeechWorker::WaitForEventForwardPauseForTest(DWORD timeoutMs)
{
    std::unique_lock<std::mutex> lock(m_eventForwardTestMutex);
    return m_eventForwardTestChanged.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
        return m_eventForwardPausedForTest || m_exit.load();
    }) && m_eventForwardPausedForTest;
}

void SpeechWorker::ReleaseEventForwardForTest()
{
    std::lock_guard<std::mutex> lock(m_eventForwardTestMutex);
    m_pauseNextEventForwardForTest = false;
    m_eventForwardPausedForTest = false;
    m_eventForwardTestChanged.notify_all();
}

void SpeechWorker::PauseNextAbortTransitionForTest()
{
    std::lock_guard<std::mutex> lock(m_abortTransitionTestMutex);
    m_pauseNextAbortTransitionForTest = true;
    m_abortTransitionPausedForTest = false;
    m_wasCancellingAtAbortUnlockForTest = false;
}

bool SpeechWorker::WaitForAbortTransitionPauseForTest(DWORD timeoutMs)
{
    std::unique_lock<std::mutex> lock(m_abortTransitionTestMutex);
    return m_abortTransitionTestChanged.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
        return m_abortTransitionPausedForTest || m_exit.load();
    }) && m_abortTransitionPausedForTest;
}

void SpeechWorker::ReleaseAbortTransitionForTest()
{
    std::lock_guard<std::mutex> lock(m_abortTransitionTestMutex);
    m_pauseNextAbortTransitionForTest = false;
    m_abortTransitionPausedForTest = false;
    m_abortTransitionTestChanged.notify_all();
}

bool SpeechWorker::WasCancellingAtAbortUnlockForTest()
{
    std::lock_guard<std::mutex> lock(m_abortTransitionTestMutex);
    return m_wasCancellingAtAbortUnlockForTest;
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
    return m_rawAudioBytesRead;
}

void SpeechWorker::PauseNextFaultPublicationForTest()
{
    std::lock_guard<std::mutex> lock(m_faultPublicationTestMutex);
    m_pauseNextFaultPublicationForTest = true;
    m_faultPublicationPausedForTest = false;
}

bool SpeechWorker::WaitForFaultPublicationPauseForTest(DWORD timeoutMs)
{
    std::unique_lock<std::mutex> lock(m_faultPublicationTestMutex);
    return m_faultPublicationTestChanged.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
        return m_faultPublicationPausedForTest || m_exit.load();
    }) && m_faultPublicationPausedForTest;
}

void SpeechWorker::ReleaseFaultPublicationForTest()
{
    std::lock_guard<std::mutex> lock(m_faultPublicationTestMutex);
    m_pauseNextFaultPublicationForTest = false;
    m_faultPublicationPausedForTest = false;
    m_faultPublicationTestChanged.notify_all();
}

void SpeechWorker::FailNextFrameAssemblyForTest()
{
    std::lock_guard<std::mutex> lock(m_requestMutex);
    m_failNextFrameAssemblyForTest = true;
}

bool SpeechWorker::IsAudioApartmentActiveForTest() const noexcept
{
    return m_audioApartmentActiveForTest.load(std::memory_order_acquire);
}
#endif

void SpeechWorker::Stop()
{
    uint64_t speakId = 0;
    {
        std::lock_guard<std::mutex> lock(m_requestMutex);
        if (m_downstreamState == DownstreamState::Idle)
        {
            return;
        }

        if (m_downstreamState == DownstreamState::Faulted || m_upstreamState == UpstreamState::Faulted)
        {
            return;
        }

        speakId = m_activeSpeakId;
        m_frameAssembler.Reset();
        m_upstreamState = UpstreamState::Idle;
        m_downstreamState = DownstreamState::Idle;
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
    if (m_downstreamState == DownstreamState::Idle)
    {
        return S_FALSE;
    }

    if (m_downstreamState == DownstreamState::Faulted || m_upstreamState == UpstreamState::Faulted)
    {
        return E_FAIL;
    }

    if (m_downstreamState == DownstreamState::Cancelling)
    {
        return E_UNEXPECTED;
    }

    speakId = m_activeSpeakId;
    m_upstreamFinished = false;
    m_upstreamTerminalBytes = 0;
    m_cancellationDeadlineTick = cancellationDeadline;
    m_frameAssembler.Reset();
    m_downstreamState = DownstreamState::Cancelling;
#if defined(_DEBUG)
    CoreLog(L"[CancelTrace] speak_id=%llu cancelling_published tick=%llu entry_to_publish_ms=%llu raw=%llu delivered=%llu.",
        speakId, GetTickCount64(), GetTickCount64() - cancellationEntryTick,
        m_rawAudioBytesRead, m_deliveredAudioBytes);
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
        return m_downstreamState != DownstreamState::Cancelling || m_exit.load();
    });

    if (!completed && m_downstreamState == DownstreamState::Cancelling)
    {
#if defined(_DEBUG)
        const uint64_t rawAudioBytes = m_rawAudioBytesRead;
        const uint64_t cancelledAudioBytes = m_upstreamTerminalBytes;
        const bool terminalReceived = m_upstreamFinished;
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
        static_cast<unsigned>(m_downstreamState), m_upstreamFinished ? 1u : 0u,
        m_upstreamTerminalBytes, m_rawAudioBytesRead);
#endif
    return m_exit.load() ? E_ABORT : m_requestCompletionHr;
}

void SpeechWorker::EnterFaultedState()
{
    {
        std::lock_guard<std::mutex> requestLock(m_requestMutex);
        m_requestCompletionHr = E_FAIL;
        m_frameAssembler.Reset();
        m_upstreamState = UpstreamState::Faulted;
        m_downstreamState = DownstreamState::Faulted;
        m_requestChanged.notify_all();

        bool expected = false;
        if (!m_faultPublicationStarted.compare_exchange_strong(expected, true))
        {
            return;
        }
        m_faultPending = true;
    }

#if defined(_DEBUG)
    {
        std::unique_lock<std::mutex> testLock(m_faultPublicationTestMutex);
        if (m_pauseNextFaultPublicationForTest)
        {
            m_pauseNextFaultPublicationForTest = false;
            m_faultPublicationPausedForTest = true;
            m_faultPublicationTestChanged.notify_all();
            m_faultPublicationTestChanged.wait(testLock, [this] {
                return !m_faultPublicationPausedForTest || m_exit.load();
            });
        }
    }
#endif

    {
        std::lock_guard<std::recursive_mutex> eventForwardLock(m_eventForwardMutex);
        m_faultVisible.store(true, std::memory_order_release);

        std::lock_guard<std::mutex> requestLock(m_requestMutex);
        m_faultPending = false;
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

    bool isLog = json.contains("event") && json["event"].is_string() && json["event"].get<std::string_view>() == "log";

    {
        std::lock_guard<std::mutex> requestLock(m_requestMutex);
        if ((m_downstreamState != DownstreamState::Speaking && !isLog) ||
            (m_faultPending && !isLog) ||
            eventSpeakId != m_activeSpeakId)
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
        stateAfterCallback = m_downstreamState;
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
    while (m_downstreamState != DownstreamState::Idle && m_downstreamState != DownstreamState::Faulted && !m_exit.load())
    {
        if (m_requestChanged.wait_for(lock, std::chrono::milliseconds(10), [this] {
            return m_downstreamState == DownstreamState::Idle || m_downstreamState == DownstreamState::Faulted || m_exit.load();
        }))
        {
            break;
        }

        const ULONGLONG now = GetTickCount64();
#if defined(_DEBUG)
        if (m_upstreamFinished && m_downstreamState == DownstreamState::Speaking &&
            (lastTerminalWaitLogTick == 0 || now - lastTerminalWaitLogTick >= 250))
        {
            CoreLog(L"[ThreadTrace] speak_id=%llu terminal_pending tick=%llu declared=%llu raw=%llu delivered=%llu carry=%u.",
                m_activeSpeakId, now, m_upstreamTerminalBytes, m_rawAudioBytesRead,
                m_deliveredAudioBytes, m_frameAssembler.HasCarry() ? 1u : 0u);
            lastTerminalWaitLogTick = now;
        }
#endif
        if (m_downstreamState == DownstreamState::Cancelling &&
            m_cancellationDeadlineTick != 0 &&
            now >= m_cancellationDeadlineTick)
        {
            const uint64_t speakId = m_activeSpeakId;
            lock.unlock();
            CoreLog(L"[SpeechWorker] Provider did not complete cancellation within %lu ms for speak_id %llu; quarantining session.",
                CancellationTimeoutMs, speakId);
            EnterFaultedState();
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }

        const ULONGLONG lastProgress = m_lastProviderProgressTick.load(std::memory_order_acquire);
        if (m_upstreamState == UpstreamState::Active &&
            now - lastProgress >= SynthesisInactivityTimeoutMs)
        {
            const uint64_t speakId = m_activeSpeakId;
            lock.unlock();
            CoreLog(L"[SpeechWorker] Provider made no progress for %llu ms during speak_id %llu; quarantining session.",
                SynthesisInactivityTimeoutMs, speakId);
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

        if ((actions & SPVES_ABORT) != 0 && m_downstreamState == DownstreamState::Speaking)
        {
            const ULONGLONG cancellationEntryTick = GetTickCount64();
            const ULONGLONG cancellationDeadline = cancellationEntryTick + CancellationTimeoutMs;
#if defined(_DEBUG)
            CoreLog(L"[CancelTrace] speak_id=%llu sapi_abort_observed tick=%llu state=%u raw=%llu delivered=%llu.",
                m_activeSpeakId, cancellationEntryTick, static_cast<unsigned>(m_downstreamState),
                m_rawAudioBytesRead, m_deliveredAudioBytes);
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
                std::lock_guard<std::mutex> testLock(m_abortTransitionTestMutex);
                m_wasCancellingAtAbortUnlockForTest = m_downstreamState == DownstreamState::Cancelling;
            }
#endif
            lock.unlock();
#if defined(_DEBUG)
            {
                std::unique_lock<std::mutex> testLock(m_abortTransitionTestMutex);
                if (m_pauseNextAbortTransitionForTest)
                {
                    m_pauseNextAbortTransitionForTest = false;
                    m_abortTransitionPausedForTest = true;
                    m_abortTransitionTestChanged.notify_all();
                    m_abortTransitionTestChanged.wait(testLock, [this] {
                        return !m_abortTransitionPausedForTest || m_exit.load();
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

    return m_requestCompletionHr;
}

bool SpeechWorker::CheckTerminalBoundaryLocked()
{
    if (m_upstreamState == UpstreamState::Failed)
    {
        m_frameAssembler.Reset();
        m_upstreamState = UpstreamState::Idle;
        m_downstreamState = DownstreamState::Idle;
        m_requestChanged.notify_all();
        return false;
    }

    if (!m_upstreamFinished)
    {
        return false;
    }

    if (m_downstreamState == DownstreamState::Speaking)
    {
        if (m_rawAudioBytesRead == m_upstreamTerminalBytes &&
            m_deliveredAudioBytes == m_upstreamTerminalBytes &&
            !m_frameAssembler.HasCarry())
        {
#if defined(_DEBUG)
            CoreLog(L"[ThreadTrace] speak_id=%llu terminal_boundary_reached tick=%llu declared=%llu raw=%llu delivered=%llu.",
                m_activeSpeakId, GetTickCount64(), m_upstreamTerminalBytes,
                m_rawAudioBytesRead, m_deliveredAudioBytes);
#endif
            m_frameAssembler.Reset();
            m_upstreamState = UpstreamState::Idle;
            m_downstreamState = DownstreamState::Idle;
            m_requestChanged.notify_all();
            return false;
        }
        else if (m_rawAudioBytesRead > m_upstreamTerminalBytes ||
                 m_deliveredAudioBytes > m_upstreamTerminalBytes ||
                 (m_rawAudioBytesRead == m_upstreamTerminalBytes &&
                  (m_deliveredAudioBytes != m_upstreamTerminalBytes || m_frameAssembler.HasCarry())))
        {
            CoreLog(L"[SpeechWorker] Provider audio boundary disagrees with raw (%llu), delivered (%llu), or carried PCM.",
                m_rawAudioBytesRead, m_deliveredAudioBytes);
            return true;
        }
    }
    else if (m_downstreamState == DownstreamState::Cancelling)
    {
        if (m_rawAudioBytesRead >= m_upstreamTerminalBytes)
        {
#if defined(_DEBUG)
            const ULONGLONG cancellationStartTick = m_cancellationDeadlineTick >= CancellationTimeoutMs
                ? m_cancellationDeadlineTick - CancellationTimeoutMs
                : 0;
            CoreLog(L"[CancelTrace] speak_id=%llu cancellation_boundary_reached tick=%llu elapsed_ms=%llu declared=%llu raw=%llu.",
                m_activeSpeakId, GetTickCount64(),
                cancellationStartTick == 0 ? 0 : GetTickCount64() - cancellationStartTick,
                m_upstreamTerminalBytes, m_rawAudioBytesRead);
#endif
            m_frameAssembler.Reset();
            m_upstreamState = UpstreamState::Idle;
            m_downstreamState = DownstreamState::Idle;
            m_requestChanged.notify_all();
            return false;
        }
    }

    return false;
}

void SpeechWorker::AudioThreadProc()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    auto apartmentCleanup = wil::scope_exit([this] {
#if defined(_DEBUG)
        m_audioApartmentActiveForTest.store(false, std::memory_order_release);
#endif
        winrt::uninit_apartment();
    });
#if defined(_DEBUG)
    m_audioApartmentActiveForTest.store(true, std::memory_order_release);
#endif
    std::vector<uint8_t> buffer(4096);
    while (!m_exit.load())
    {
        DWORD bytesRead = 0;
        HRESULT hr = m_pClient->ReadAudioChunk(buffer, bytesRead);
        if (FAILED(hr))
        {
            if (m_exit.load()) break;
            CoreLog(L"[SpeechWorker] Audio pipe failed; quarantining provider session: 0x%08x.", hr);
            EnterFaultedState();
            break;
        }

        if (bytesRead == 0)
        {
            continue;
        }

        uint64_t cancellationToSend = 0;
        bool protocolBoundaryFailed = false;
        bool shouldDeliverAudio = false;
        PcmFrameBatch spansToWrite{};

        {
            std::lock_guard<std::mutex> lock(m_requestMutex);
            if (m_downstreamState == DownstreamState::Speaking || m_downstreamState == DownstreamState::Cancelling)
            {
                m_lastProviderProgressTick.store(GetTickCount64(), std::memory_order_release);
            }
            if (m_downstreamState == DownstreamState::Speaking)
            {
                size_t bytesToFrame = bytesRead;
                if (m_upstreamFinished)
                {
                    const uint64_t remainingDeclaredBytes = m_upstreamTerminalBytes > m_rawAudioBytesRead
                        ? m_upstreamTerminalBytes - m_rawAudioBytesRead
                        : 0;
                    bytesToFrame = static_cast<size_t>((std::min)(static_cast<uint64_t>(bytesRead), remainingDeclaredBytes));
                }
                m_rawAudioBytesRead += bytesRead;
#if defined(_DEBUG)
                if (m_failNextFrameAssemblyForTest)
                {
                    m_failNextFrameAssemblyForTest = false;
                    throw std::bad_alloc();
                }
#endif
                spansToWrite = m_frameAssembler.Process(buffer.data(), bytesToFrame);
                if (spansToWrite.empty())
                {
                    protocolBoundaryFailed = CheckTerminalBoundaryLocked();
                }
                else
                {
                    shouldDeliverAudio = true;
                }
            }
            else if (m_downstreamState == DownstreamState::Cancelling)
            {
#if defined(_DEBUG)
                const uint64_t rawBeforeRead = m_rawAudioBytesRead;
#endif
                m_rawAudioBytesRead += bytesRead;
#if defined(_DEBUG)
                if (m_upstreamFinished)
                {
                    CoreLog(L"[CancelTrace] speak_id=%llu cancellation_audio_read tick=%llu chunk=%lu raw_before=%llu raw_after=%llu declared=%llu.",
                        m_activeSpeakId, GetTickCount64(), bytesRead, rawBeforeRead,
                        m_rawAudioBytesRead, m_upstreamTerminalBytes);
                }
#endif
                protocolBoundaryFailed = CheckTerminalBoundaryLocked();
            }
            else if (m_downstreamState == DownstreamState::Idle)
            {
                CoreLog(L"[SpeechWorker] Provider sent audio after the active request reached its terminal boundary.");
                protocolBoundaryFailed = true;
            }
            else if (m_downstreamState == DownstreamState::Faulted)
            {
                // Continue draining provider PCM so its audio pipe cannot fill, but never call SAPI.
            }

            if (protocolBoundaryFailed)
            {
                m_faultPending = true;
            }
        }

        if (shouldDeliverAudio && !spansToWrite.empty())
        {
            bool writeAccepted = true;
            size_t totalBytesDeliveredInBatch = 0;

            for (const auto& span : spansToWrite)
            {
                if (!m_pEngine->OnAudioData(span.data, static_cast<uint32_t>(span.size)))
                {
                    writeAccepted = false;
                    break;
                }
                totalBytesDeliveredInBatch += span.size;
            }

            {
                std::lock_guard<std::mutex> lock(m_requestMutex);
                if (m_downstreamState == DownstreamState::Speaking)
                {
                    m_deliveredAudioBytes += totalBytesDeliveredInBatch;
                    if (!writeAccepted)
                    {
                        CoreLog(L"[SpeechWorker] SAPI rejected an audio write; cancelling active synthesis.");
                        cancellationToSend = m_activeSpeakId;
                        m_upstreamFinished = false;
                        m_upstreamTerminalBytes = 0;
                        m_cancellationDeadlineTick = GetTickCount64() + CancellationTimeoutMs;
                        m_frameAssembler.Reset();
                        m_downstreamState = DownstreamState::Cancelling;
                    }
                    else
                    {
                        protocolBoundaryFailed = CheckTerminalBoundaryLocked();
                    }
                }
                else if (m_downstreamState == DownstreamState::Cancelling)
                {
                    protocolBoundaryFailed = CheckTerminalBoundaryLocked();
                }

                if (protocolBoundaryFailed)
                {
                    m_faultPending = true;
                }
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

        if (protocolBoundaryFailed)
        {
            EnterFaultedState();
        }
    }
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
            if (m_exit.load()) break;
            CoreLog(L"[SpeechWorker] Control pipe failed; quarantining provider session: 0x%08x.", hr);
            EnterFaultedState();
            break;
        }

        if (json.is_null() || !json.is_object())
        {
            continue;
        }

        try
        {
            bool forwardToSapi = false;
            bool faultAfterStateUpdate = false;
            if (json.contains("event"))
            {
                if (json["event"].is_string())
                {
                    std::string_view eventStr = json["event"].get<std::string_view>();
                    uint64_t eventSpeakId = 0;
                    if (!json.contains("speak_id") || !TryGetJsonUnsignedInteger(json["speak_id"], eventSpeakId))
                    {
                        std::lock_guard<std::mutex> lock(m_requestMutex);
                        if (m_upstreamState != UpstreamState::Faulted && m_downstreamState != DownstreamState::Faulted)
                        {
                            m_faultPending = true;
                            faultAfterStateUpdate = true;
                        }
                    }
                    else
                    {
                        const bool isWordBoundary = eventStr == "word_boundary";
                        const bool isSentenceBoundary = eventStr == "sentence_boundary";
                        const bool isBookmarkReached = eventStr == "bookmark_reached";
                        const bool isSpeechEvent = isWordBoundary || isSentenceBoundary || isBookmarkReached;
                        uint32_t audioOffsetMs = 0;
                        uint32_t textOffset = 0;
                        uint32_t textLength = 0;
                        const bool hasValidSpeechEventNumbers = !isSpeechEvent ||
                            (json.contains("audio_offset_ms") &&
                             TryGetJsonUnsignedInteger(json["audio_offset_ms"], audioOffsetMs) &&
                             (isBookmarkReached ||
                              (json.contains("text_offset") && TryGetJsonUnsignedInteger(json["text_offset"], textOffset) &&
                               json.contains("text_length") && TryGetJsonUnsignedInteger(json["text_length"], textLength))));
                        const bool isSynthesisComplete = eventStr == "synthesis_complete";
                        const bool isSynthesisCancelled = eventStr == "synthesis_cancelled";
                        uint64_t terminalAudioBytes = 0;
                        const bool hasValidTerminalAudioBytes =
                            (!isSynthesisComplete && !isSynthesisCancelled) ||
                            ((isSynthesisComplete ? json.contains("total_audio_bytes") : json.contains("audio_bytes_written")) &&
                             TryGetJsonUnsignedInteger(
                                 json[isSynthesisComplete ? "total_audio_bytes" : "audio_bytes_written"], terminalAudioBytes));

                        std::lock_guard<std::mutex> lock(m_requestMutex);
                        forwardToSapi = m_upstreamState != UpstreamState::Faulted && m_downstreamState != DownstreamState::Faulted && !m_faultPending;
                        if (eventSpeakId == m_activeSpeakId)
                        {
                            const bool isProviderProgress = eventStr == "word_boundary" ||
                                eventStr == "sentence_boundary" || eventStr == "bookmark_reached" ||
                                eventStr == "synthesis_complete" || eventStr == "synthesis_cancelled" ||
                                eventStr == "log";
                            if (isProviderProgress && hasValidSpeechEventNumbers && hasValidTerminalAudioBytes &&
                                (m_downstreamState == DownstreamState::Speaking || m_downstreamState == DownstreamState::Cancelling))
                            {
                                m_lastProviderProgressTick.store(GetTickCount64(), std::memory_order_release);
                            }

                            if (isSpeechEvent && !hasValidSpeechEventNumbers)
                            {
                                forwardToSapi = false;
                                faultAfterStateUpdate = true;
                            }
                            else if (isSpeechEvent && m_downstreamState != DownstreamState::Speaking)
                            {
                                // SAPI has aborted this request, so delayed provider callbacks must not move focus.
                                forwardToSapi = false;
                            }

                            if (eventStr == "synthesis_complete" || eventStr == "synthesis_cancelled")
                            {
                                if (m_upstreamFinished)
                                {
                                    CoreLog(L"[SpeechWorker] Duplicate terminal event for speak_id %llu.", eventSpeakId);
                                    m_upstreamState = UpstreamState::Faulted;
                                    m_downstreamState = DownstreamState::Faulted;
                                    m_requestCompletionHr = E_FAIL;
                                    m_requestChanged.notify_all();
                                    faultAfterStateUpdate = true;
                                }
                                else if (!hasValidTerminalAudioBytes)
                                {
                                    CoreLog(L"[SpeechWorker] terminal event for speak_id %llu has an invalid audio bytes value.", eventSpeakId);
                                    m_upstreamState = UpstreamState::Faulted;
                                    m_downstreamState = DownstreamState::Faulted;
                                    m_requestCompletionHr = E_FAIL;
                                    m_requestChanged.notify_all();
                                    faultAfterStateUpdate = true;
                                }
                                else
                                {
                                    m_upstreamTerminalBytes = terminalAudioBytes;
                                    if (m_upstreamTerminalBytes % m_frameAssembler.BlockAlign() != 0)
                                    {
                                        CoreLog(L"[SpeechWorker] terminal event for speak_id %llu is not PCM-frame aligned.", eventSpeakId);
                                        m_upstreamState = UpstreamState::Faulted;
                                        m_downstreamState = DownstreamState::Faulted;
                                        m_requestCompletionHr = E_FAIL;
                                        m_requestChanged.notify_all();
                                        faultAfterStateUpdate = true;
                                    }
                                    else
                                    {
                                        m_upstreamFinished = true;
                                        m_upstreamState = isSynthesisComplete ? UpstreamState::Completed : UpstreamState::Cancelled;
#if defined(_DEBUG)
                                        CoreLog(L"[ThreadTrace] speak_id=%llu terminal_received tick=%llu event=%hs declared=%llu raw=%llu delivered=%llu carry=%u downstream=%u.",
                                            eventSpeakId, GetTickCount64(), eventStr.data(), m_upstreamTerminalBytes,
                                            m_rawAudioBytesRead, m_deliveredAudioBytes,
                                            m_frameAssembler.HasCarry() ? 1u : 0u,
                                            static_cast<unsigned>(m_downstreamState));
#endif
                                        faultAfterStateUpdate = CheckTerminalBoundaryLocked();
                                    }
                                }
                            }
                            else if (eventStr == "completed")
                            {
                                CoreLog(L"[SpeechWorker] Ignoring legacy completed event for speak_id %llu.", eventSpeakId);
                            }
                            else if (eventStr == "log")
                            {
                                std::string_view severity = "info";
                                if (json.contains("severity") && json["severity"].is_string())
                                {
                                    severity = json["severity"].get<std::string_view>();
                                }

                                std::string_view message = "";
                                if (json.contains("message") && json["message"].is_string())
                                {
                                    message = json["message"].get<std::string_view>();
                                }

                                CoreLog(L"[SpeechWorker] Provider log (severity=%.*hs, speak_id=%llu): %.*hs",
                                    static_cast<int>(severity.size()), severity.data(),
                                    eventSpeakId,
                                    static_cast<int>(message.size()), message.data());

                                if (severity == "fatal")
                                {
                                    faultAfterStateUpdate = true;
                                }
                                else if (severity == "error")
                                {
                                    m_upstreamState = UpstreamState::Failed;
                                    m_upstreamFinished = true;
                                    m_requestCompletionHr = E_FAIL;
                                    m_upstreamTerminalBytes = m_rawAudioBytesRead;
                                    faultAfterStateUpdate = CheckTerminalBoundaryLocked();
                                }
                            }

                            if (faultAfterStateUpdate)
                            {
                                m_faultPending = true;
                            }
                        }
                    }
                }
            }

            if (forwardToSapi)
            {
#if defined(_DEBUG)
                {
                    std::unique_lock<std::mutex> testLock(m_eventForwardTestMutex);
                    if (m_pauseNextEventForwardForTest)
                    {
                        m_pauseNextEventForwardForTest = false;
                        m_eventForwardPausedForTest = true;
                        m_eventForwardTestChanged.notify_all();
                        m_eventForwardTestChanged.wait(testLock, [this] {
                            return !m_eventForwardPausedForTest || m_exit.load();
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
