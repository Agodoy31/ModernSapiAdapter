#include "pch.h"
#include "SpeechWorker.h"
#include "SapiEngine.h"

SpeechWorker::SpeechWorker(CSapiEngine* pEngine, PipeClient* pClient)
    : m_pEngine(pEngine), m_pClient(pClient), m_exit(false)
{
    m_audioThread = std::thread(&SpeechWorker::AudioThreadProc, this);
    m_controlThread = std::thread(&SpeechWorker::ControlThreadProc, this);
}

SpeechWorker::~SpeechWorker()
{
    m_exit.store(true);
#if defined(_DEBUG)
    ReleaseEventForwardForTest();
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
    if (m_requestState == RequestState::Faulted)
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
    m_requestState = RequestState::Speaking;
    return true;
}

bool SpeechWorker::IsFaulted() const
{
    std::lock_guard<std::mutex> lock(m_requestMutex);
    return m_requestState == RequestState::Faulted;
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
        m_requestState = RequestState::Idle;
        m_requestChanged.notify_all();
    }

    SendCancellation(speakId);
}

HRESULT SpeechWorker::CancelAndDrain()
{
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
        m_requestState = RequestState::Cancelling;
    }

    HRESULT hr = SendCancellation(speakId);
    if (FAILED(hr))
    {
        EnterFaultedState();
        return E_FAIL;
    }

    std::unique_lock<std::mutex> lock(m_requestMutex);
    m_requestChanged.wait(lock, [this] {
        return m_requestState != RequestState::Cancelling || m_exit.load();
    });

    return m_cancellationFailed ? E_FAIL : S_OK;
}

void SpeechWorker::EnterFaultedState()
{
    std::lock_guard<std::recursive_mutex> eventForwardLock(m_eventForwardMutex);
    m_faultVisible.store(true, std::memory_order_release);

    std::lock_guard<std::mutex> requestLock(m_requestMutex);
    m_cancellationFailed = true;
    m_requestState = RequestState::Faulted;
    m_requestChanged.notify_all();
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

HRESULT SpeechWorker::SendCancellation(uint64_t speakId)
{
    try
    {
        using namespace winrt::Windows::Data::Json;
        JsonObject req;
        req.SetNamedValue(L"command", JsonValue::CreateStringValue(L"cancel"));
        req.SetNamedValue(L"speak_id", JsonValue::CreateNumberValue(static_cast<double>(speakId)));
        return m_pClient ? m_pClient->SendControlMessage(req) : E_UNEXPECTED;
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

void SpeechWorker::CompleteIfAudioBoundaryReached()
{
    if (!m_synthesisComplete || m_requestState != RequestState::Speaking)
    {
        return;
    }

    if (m_deliveredAudioBytes == m_expectedAudioBytes)
    {
        m_requestState = RequestState::Idle;
        m_requestChanged.notify_all();
    }
    else if (m_deliveredAudioBytes > m_expectedAudioBytes)
    {
        CoreLog(L"[SpeechWorker] Provider declared %llu audio bytes after %llu bytes were already delivered.",
            m_expectedAudioBytes, m_deliveredAudioBytes);
        m_requestState = RequestState::Idle;
        m_requestChanged.notify_all();
    }
}

bool SpeechWorker::CompleteCancellationIfAudioBoundaryReached()
{
    if (!m_cancellationComplete || m_requestState != RequestState::Cancelling)
    {
        return false;
    }

    if (m_rawAudioBytesRead == m_cancelledAudioBytes)
    {
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
    std::vector<uint8_t> buffer(4096);
    while (!m_exit.load())
    {
        DWORD bytesRead = 0;
        HRESULT hr = m_pClient->ReadAudioChunk(buffer, bytesRead);
        if (FAILED(hr))
        {
            if (m_exit.load()) break;
            Sleep(50);
            if (m_exit.load()) break;
            continue;
        }

        if (bytesRead == 0)
        {
            continue;
        }

        uint64_t cancellationToSend = 0;
        bool cancellationBoundaryFailed = false;
        {
            std::lock_guard<std::mutex> lock(m_requestMutex);
            if (m_requestState == RequestState::Speaking)
            {
                m_rawAudioBytesRead += bytesRead;
                if (!m_pEngine->OnAudioData(buffer.data(), bytesRead))
                {
                    CoreLog(L"[SpeechWorker] SAPI rejected an audio write; cancelling active synthesis.");
                    cancellationToSend = m_activeSpeakId;
                    m_cancelledAudioBytes = 0;
                    m_cancellationComplete = false;
                    m_cancellationFailed = false;
                    m_requestState = RequestState::Cancelling;
                }
                else
                {
                    m_deliveredAudioBytes += bytesRead;
                    CompleteIfAudioBoundaryReached();
                }
            }
            else if (m_requestState == RequestState::Cancelling)
            {
                m_rawAudioBytesRead += bytesRead;
                cancellationBoundaryFailed = CompleteCancellationIfAudioBoundaryReached();
            }
            else if (m_requestState == RequestState::Faulted)
            {
                // Continue draining provider PCM so its audio pipe cannot fill, but never call SAPI.
            }
        }

        if (cancellationToSend != 0)
        {
            const HRESULT cancellationHr = SendCancellation(cancellationToSend);
            if (FAILED(cancellationHr))
            {
                CoreLog(L"[SpeechWorker] Failed to cancel speak_id %llu after SAPI rejected audio: 0x%08x.",
                    cancellationToSend, cancellationHr);

                EnterFaultedState();
            }
        }

        if (cancellationBoundaryFailed)
        {
            EnterFaultedState();
        }
    }
    winrt::uninit_apartment();
}

void SpeechWorker::ControlThreadProc()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    while (!m_exit.load())
    {
        winrt::Windows::Data::Json::JsonObject json = nullptr;
        HRESULT hr = m_pClient->ReadControlMessage(json);
        if (FAILED(hr) || !json)
        {
            if (m_exit.load()) break;
            Sleep(50);
            if (m_exit.load()) break;
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
                        forwardToSapi = m_requestState != RequestState::Faulted;
                        if (eventSpeakId == m_activeSpeakId)
                        {
                            constexpr double maxExactJsonInteger = 9007199254740991.0;
                            const bool isSpeechEvent = eventStr == L"word_boundary" ||
                                eventStr == L"sentence_boundary" || eventStr == L"bookmark_reached";
                            if (isSpeechEvent && m_requestState != RequestState::Speaking)
                            {
                                // SAPI has aborted this request, so delayed provider callbacks must not move focus.
                                forwardToSapi = false;
                            }

                            if (eventStr == L"synthesis_complete" && m_requestState == RequestState::Speaking)
                            {
                                if (!json.HasKey(L"total_audio_bytes") ||
                                    json.GetNamedValue(L"total_audio_bytes").ValueType() != winrt::Windows::Data::Json::JsonValueType::Number)
                                {
                                    CoreLog(L"[SpeechWorker] synthesis_complete for speak_id %llu omitted total_audio_bytes.", eventSpeakId);
                                    m_requestState = RequestState::Idle;
                                    m_requestChanged.notify_all();
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
                                        m_requestState = RequestState::Idle;
                                        m_requestChanged.notify_all();
                                    }
                                    else
                                    {
                                        m_expectedAudioBytes = static_cast<uint64_t>(totalAudioBytesValue);
                                        m_synthesisComplete = true;
                                        CompleteIfAudioBoundaryReached();
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
                                        m_cancellationComplete = true;
                                        faultAfterStateUpdate = CompleteCancellationIfAudioBoundaryReached();
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
                                        m_requestState = RequestState::Idle;
                                        m_requestChanged.notify_all();
                                    }
                                }
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
