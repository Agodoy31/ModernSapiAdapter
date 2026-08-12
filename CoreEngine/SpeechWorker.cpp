#include "pch.h"
#include "SpeechWorker.h"
#include "SapiEngine.h"

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
    m_controlThread = std::thread(&SpeechWorker::ControlThreadProc, this);
}

SpeechWorker::~SpeechWorker()
{
    m_exit.store(true);
#if defined(_DEBUG)
    ReleaseEventForwardForTest();
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
    if (m_requestState != RequestState::Idle || m_faultPending)
    {
        return false;
    }

    m_activeSpeakId = speakId;
    m_deliveredAudioBytes = 0;
    m_rawAudioBytesRead = 0;
    m_expectedAudioBytes = 0;
    m_cancelledAudioBytes = 0;
    m_synthesisComplete = false;
    m_cancellationComplete = false;
    m_cancellationFailed = false;
    m_cancellationDeadlineTick = 0;
    m_frameAssembler.Reset();
    m_lastProviderProgressTick.store(GetTickCount64(), std::memory_order_release);
    m_requestState = RequestState::Speaking;
    return true;
}

bool SpeechWorker::IsFaulted() const
{
    std::lock_guard<std::mutex> lock(m_requestMutex);
    return m_requestState == RequestState::Faulted || m_faultPending;
}

#if defined(_DEBUG)
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
        if (m_requestState == RequestState::Idle)
        {
            return;
        }

        if (m_requestState == RequestState::Faulted)
        {
            return;
        }

        speakId = m_activeSpeakId;
        m_frameAssembler.Reset();
        m_requestState = RequestState::Idle;
        m_requestChanged.notify_all();
    }

    SendCancellation(speakId, CancellationTimeoutMs);
}

HRESULT SpeechWorker::CancelAndDrain()
{
    const ULONGLONG cancellationDeadline = GetTickCount64() + CancellationTimeoutMs;
    uint64_t speakId = 0;
    {
        std::lock_guard<std::mutex> lock(m_requestMutex);
        if (m_requestState == RequestState::Idle)
        {
            return S_OK;
        }

        if (m_requestState == RequestState::Faulted)
        {
            return E_FAIL;
        }

        if (m_requestState == RequestState::Cancelling)
        {
            return E_UNEXPECTED;
        }

        speakId = m_activeSpeakId;
        m_cancellationComplete = false;
        m_cancellationFailed = false;
        m_cancellationDeadlineTick = cancellationDeadline;
        m_frameAssembler.Reset();
        m_requestState = RequestState::Cancelling;
    }

    const ULONGLONG beforeSend = GetTickCount64();
    const DWORD sendTimeout = beforeSend < cancellationDeadline
        ? static_cast<DWORD>(cancellationDeadline - beforeSend)
        : 0;
    HRESULT hr = SendCancellation(speakId, sendTimeout);
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
        return m_requestState != RequestState::Cancelling || m_exit.load();
    });

    if (!completed && m_requestState == RequestState::Cancelling)
    {
        lock.unlock();
        CoreLog(L"[SpeechWorker] Provider did not complete cancellation within %lu ms for speak_id %llu; quarantining session.",
            CancellationTimeoutMs, speakId);
        EnterFaultedState();
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }

    return m_exit.load() ? E_ABORT : (m_cancellationFailed ? E_FAIL : S_OK);
}

void SpeechWorker::EnterFaultedState()
{
    {
        std::lock_guard<std::mutex> requestLock(m_requestMutex);
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
        m_cancellationFailed = true;
        m_frameAssembler.Reset();
        m_requestState = RequestState::Faulted;
        m_faultPending = false;
        m_requestChanged.notify_all();
    }

    if (m_pClient)
    {
        m_pClient->Cancel();
    }
}

void SpeechWorker::ForwardEventToSapi(const winrt::Windows::Data::Json::JsonObject& json)
{
    std::lock_guard<std::recursive_mutex> eventForwardLock(m_eventForwardMutex);
    if (m_faultVisible.load(std::memory_order_acquire))
    {
        return;
    }

    m_pEngine->OnSpeechEvent(json);
}

HRESULT SpeechWorker::SendCancellation(uint64_t speakId, DWORD timeoutMs)
{
    try
    {
        using namespace winrt::Windows::Data::Json;
        JsonObject req;
        req.SetNamedValue(L"command", JsonValue::CreateStringValue(L"cancel"));
        req.SetNamedValue(L"speak_id", JsonValue::CreateNumberValue(static_cast<double>(speakId)));
        return m_pClient ? m_pClient->SendControlMessage(req, timeoutMs) : E_UNEXPECTED;
    }
    catch (const winrt::hresult_error& e)
    {
        CoreLog(L"[SpeechWorker] WinRT exception while cancelling speak_id %llu: 0x%08x - %s", speakId, e.code().value, e.message().c_str());
        return e.code();
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
    while (m_requestState != RequestState::Idle && m_requestState != RequestState::Faulted && !m_exit.load())
    {
        if (m_requestChanged.wait_for(lock, std::chrono::milliseconds(10), [this] {
            return m_requestState == RequestState::Idle || m_requestState == RequestState::Faulted || m_exit.load();
        }))
        {
            break;
        }

        const ULONGLONG now = GetTickCount64();
        if (m_requestState == RequestState::Cancelling &&
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
        if (m_requestState == RequestState::Speaking &&
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

        if ((actions & SPVES_ABORT) != 0 && m_requestState != RequestState::Idle && m_requestState != RequestState::Faulted)
        {
            lock.unlock();
            CoreLog(L"[SpeechWorker] SAPI requested SPVES_ABORT; cancelling active synthesis.");
            return CancelAndDrain();
        }
    }

    if (m_cancellationFailed)
    {
        return E_FAIL;
    }

    if (m_exit.load())
    {
        return E_ABORT;
    }

    return S_OK;
}

bool SpeechWorker::CompleteIfAudioBoundaryReached()
{
    if (!m_synthesisComplete || m_requestState != RequestState::Speaking)
    {
        return false;
    }

    if (m_rawAudioBytesRead == m_expectedAudioBytes &&
        m_deliveredAudioBytes == m_expectedAudioBytes &&
        !m_frameAssembler.HasCarry())
    {
        m_frameAssembler.Reset();
        m_requestState = RequestState::Idle;
        m_requestChanged.notify_all();
        return false;
    }
    else if (m_rawAudioBytesRead > m_expectedAudioBytes ||
             m_deliveredAudioBytes > m_expectedAudioBytes ||
             (m_rawAudioBytesRead == m_expectedAudioBytes &&
              (m_deliveredAudioBytes != m_expectedAudioBytes || m_frameAssembler.HasCarry())))
    {
        CoreLog(L"[SpeechWorker] Provider audio boundary disagrees with raw (%llu), delivered (%llu), or carried PCM.",
            m_rawAudioBytesRead, m_deliveredAudioBytes);
        return true;
    }

    return false;
}

bool SpeechWorker::CompleteCancellationIfAudioBoundaryReached()
{
    if (!m_cancellationComplete || m_requestState != RequestState::Cancelling)
    {
        return false;
    }

    if (m_rawAudioBytesRead == m_cancelledAudioBytes)
    {
        m_frameAssembler.Reset();
        m_requestState = RequestState::Idle;
        m_requestChanged.notify_all();
        return false;
    }
    else if (m_rawAudioBytesRead > m_cancelledAudioBytes)
    {
        CoreLog(L"[SpeechWorker] Provider declared %llu cancellation bytes after %llu bytes were already read.",
            m_cancelledAudioBytes, m_rawAudioBytesRead);
        return true;
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
        {
            std::lock_guard<std::mutex> lock(m_requestMutex);
            if (m_requestState == RequestState::Speaking || m_requestState == RequestState::Cancelling)
            {
                m_lastProviderProgressTick.store(GetTickCount64(), std::memory_order_release);
            }
            if (m_requestState == RequestState::Speaking)
            {
                size_t bytesToFrame = bytesRead;
                if (m_synthesisComplete)
                {
                    const uint64_t remainingDeclaredBytes = m_expectedAudioBytes > m_rawAudioBytesRead
                        ? m_expectedAudioBytes - m_rawAudioBytesRead
                        : 0;
                    bytesToFrame = static_cast<size_t>((std::min)(static_cast<uint64_t>(bytesRead), remainingDeclaredBytes));
                }
                m_rawAudioBytesRead += bytesRead;
                bool writeAccepted = true;
#if defined(_DEBUG)
                if (m_failNextFrameAssemblyForTest)
                {
                    m_failNextFrameAssemblyForTest = false;
                    throw std::bad_alloc();
                }
#endif
                const auto spans = m_frameAssembler.Process(buffer.data(), bytesToFrame);
                for (const auto& span : spans)
                {
                    if (!m_pEngine->OnAudioData(span.data, static_cast<uint32_t>(span.size)))
                    {
                        writeAccepted = false;
                        break;
                    }
                    m_deliveredAudioBytes += span.size;
                }

                if (!writeAccepted)
                {
                    CoreLog(L"[SpeechWorker] SAPI rejected an audio write; cancelling active synthesis.");
                    cancellationToSend = m_activeSpeakId;
                    m_cancelledAudioBytes = 0;
                    m_cancellationComplete = false;
                    m_cancellationFailed = false;
                    m_cancellationDeadlineTick = GetTickCount64() + CancellationTimeoutMs;
                    m_frameAssembler.Reset();
                    m_requestState = RequestState::Cancelling;
                }
                else
                {
                    protocolBoundaryFailed = CompleteIfAudioBoundaryReached();
                }
            }
            else if (m_requestState == RequestState::Cancelling)
            {
                m_rawAudioBytesRead += bytesRead;
                protocolBoundaryFailed = CompleteCancellationIfAudioBoundaryReached();
            }
            else if (m_requestState == RequestState::Idle)
            {
                CoreLog(L"[SpeechWorker] Provider sent audio after the active request reached its terminal boundary.");
                protocolBoundaryFailed = true;
            }
            else if (m_requestState == RequestState::Faulted)
            {
                // Continue draining provider PCM so its audio pipe cannot fill, but never call SAPI.
            }

            if (protocolBoundaryFailed)
            {
                m_faultPending = true;
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
        winrt::Windows::Data::Json::JsonObject json = nullptr;
        HRESULT hr = m_pClient->ReadControlMessage(json);
        if (FAILED(hr))
        {
            if (m_exit.load()) break;
            CoreLog(L"[SpeechWorker] Control pipe failed; quarantining provider session: 0x%08x.", hr);
            EnterFaultedState();
            break;
        }

        if (!json)
        {
            continue;
        }

        try
        {
            bool forwardToSapi = false;
            bool faultAfterStateUpdate = false;
            if (json.HasKey(L"event") && json.HasKey(L"speak_id"))
            {
                if (json.GetNamedValue(L"event").ValueType() == winrt::Windows::Data::Json::JsonValueType::String &&
                    json.GetNamedValue(L"speak_id").ValueType() == winrt::Windows::Data::Json::JsonValueType::Number)
                {
                    auto eventStr = json.GetNamedString(L"event");
                    uint64_t eventSpeakId = static_cast<uint64_t>(json.GetNamedNumber(L"speak_id"));

                    {
                        std::lock_guard<std::mutex> lock(m_requestMutex);
                        forwardToSapi = m_requestState != RequestState::Faulted && !m_faultPending;
                        if (eventSpeakId == m_activeSpeakId)
                        {
                            const bool isProviderProgress = eventStr == L"word_boundary" ||
                                eventStr == L"sentence_boundary" || eventStr == L"bookmark_reached" ||
                                eventStr == L"synthesis_complete" || eventStr == L"synthesis_cancelled" ||
                                eventStr == L"log";
                            if (isProviderProgress &&
                                (m_requestState == RequestState::Speaking || m_requestState == RequestState::Cancelling))
                            {
                                m_lastProviderProgressTick.store(GetTickCount64(), std::memory_order_release);
                            }

                            constexpr double maxExactJsonInteger = 9007199254740991.0;
                            const bool isSpeechEvent = eventStr == L"word_boundary" ||
                                eventStr == L"sentence_boundary" || eventStr == L"bookmark_reached";
                            if (isSpeechEvent && m_requestState != RequestState::Speaking)
                            {
                                // SAPI has aborted this request, so delayed provider callbacks must not move focus.
                                forwardToSapi = false;
                            }

                            if (eventStr == L"synthesis_complete" &&
                                m_requestState == RequestState::Idle && m_synthesisComplete)
                            {
                                CoreLog(L"[SpeechWorker] Duplicate synthesis_complete for completed speak_id %llu.", eventSpeakId);
                                faultAfterStateUpdate = true;
                            }
                            else if (eventStr == L"synthesis_complete" && m_requestState == RequestState::Speaking)
                            {
                                if (!json.HasKey(L"total_audio_bytes") ||
                                    json.GetNamedValue(L"total_audio_bytes").ValueType() != winrt::Windows::Data::Json::JsonValueType::Number)
                                {
                                    CoreLog(L"[SpeechWorker] synthesis_complete for speak_id %llu omitted total_audio_bytes.", eventSpeakId);
                                    faultAfterStateUpdate = true;
                                }
                                else
                                {
                                    const double totalAudioBytesValue = json.GetNamedNumber(L"total_audio_bytes");
                                    if (!std::isfinite(totalAudioBytesValue) || totalAudioBytesValue < 0 ||
                                        totalAudioBytesValue > maxExactJsonInteger ||
                                        totalAudioBytesValue != static_cast<double>(static_cast<uint64_t>(totalAudioBytesValue)) ||
                                        m_synthesisComplete)
                                    {
                                        CoreLog(L"[SpeechWorker] synthesis_complete for speak_id %llu has an invalid or duplicate total_audio_bytes value.", eventSpeakId);
                                        faultAfterStateUpdate = true;
                                    }
                                    else
                                    {
                                        m_expectedAudioBytes = static_cast<uint64_t>(totalAudioBytesValue);
                                        if (m_expectedAudioBytes % m_frameAssembler.BlockAlign() != 0)
                                        {
                                            CoreLog(L"[SpeechWorker] synthesis_complete for speak_id %llu is not PCM-frame aligned.", eventSpeakId);
                                            faultAfterStateUpdate = true;
                                        }
                                        else
                                        {
                                            m_synthesisComplete = true;
                                            faultAfterStateUpdate = CompleteIfAudioBoundaryReached();
                                        }
                                    }
                                }
                            }
                            else if (eventStr == L"synthesis_cancelled" && m_requestState == RequestState::Cancelling)
                            {
                                if (!json.HasKey(L"audio_bytes_written") ||
                                    json.GetNamedValue(L"audio_bytes_written").ValueType() != winrt::Windows::Data::Json::JsonValueType::Number)
                                {
                                    CoreLog(L"[SpeechWorker] synthesis_cancelled for speak_id %llu omitted audio_bytes_written.", eventSpeakId);
                                    faultAfterStateUpdate = true;
                                }
                                else
                                {
                                    const double cancelledAudioBytesValue = json.GetNamedNumber(L"audio_bytes_written");
                                    if (!std::isfinite(cancelledAudioBytesValue) || cancelledAudioBytesValue < 0 ||
                                        cancelledAudioBytesValue > maxExactJsonInteger ||
                                        cancelledAudioBytesValue != static_cast<double>(static_cast<uint64_t>(cancelledAudioBytesValue)) ||
                                        m_cancellationComplete)
                                    {
                                        CoreLog(L"[SpeechWorker] synthesis_cancelled for speak_id %llu has an invalid or duplicate audio_bytes_written value.", eventSpeakId);
                                        faultAfterStateUpdate = true;
                                    }
                                    else
                                    {
                                        m_cancelledAudioBytes = static_cast<uint64_t>(cancelledAudioBytesValue);
                                        if (m_cancelledAudioBytes % m_frameAssembler.BlockAlign() != 0)
                                        {
                                            CoreLog(L"[SpeechWorker] synthesis_cancelled for speak_id %llu is not PCM-frame aligned.", eventSpeakId);
                                            faultAfterStateUpdate = true;
                                        }
                                        else
                                        {
                                            m_cancellationComplete = true;
                                            faultAfterStateUpdate = CompleteCancellationIfAudioBoundaryReached();
                                        }
                                    }
                                }
                            }
                            else if (eventStr == L"completed")
                            {
                                CoreLog(L"[SpeechWorker] Ignoring legacy completed event for speak_id %llu.", eventSpeakId);
                            }
                            else if (eventStr == L"log")
                            {
                                const bool fatal = !json.HasKey(L"severity") ||
                                    json.GetNamedValue(L"severity").ValueType() != winrt::Windows::Data::Json::JsonValueType::String ||
                                    json.GetNamedString(L"severity") == L"error" ||
                                    json.GetNamedString(L"severity") == L"fatal";
                                if (fatal)
                                {
                                    if (m_requestState == RequestState::Cancelling)
                                    {
                                        faultAfterStateUpdate = true;
                                    }
                                    else
                                    {
                                        m_frameAssembler.Reset();
                                        m_requestState = RequestState::Idle;
                                        m_requestChanged.notify_all();
                                    }
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

            if (faultAfterStateUpdate)
            {
                EnterFaultedState();
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
        }
        catch (const winrt::hresult_error& e)
        {
            CoreLog(L"[SpeechWorker] WinRT exception in ControlThreadProc: 0x%08x - %s", e.code().value, e.message().c_str());
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
