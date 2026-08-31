#include "pch.h"
#include "SapiEngine.h"
#include "AudioFormatUtils.h"
#include "JsonValue.h"

namespace {
    std::string WideToUtf8(const wchar_t* wstr, size_t len) {
        if (len == 0 || !wstr) return "";
        std::string utf8(len * 3, '\0');
        int written = WideCharToMultiByte(CP_UTF8, 0, wstr, static_cast<int>(len), utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
        if (written > 0) {
            utf8.resize(written);
            return utf8;
        }
        return "";
    }

    std::wstring Utf8ToWide(const std::string& utf8Str) {
        if (utf8Str.empty()) return L"";
        std::wstring wide(utf8Str.size(), L'\0');
        int written = MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), static_cast<int>(utf8Str.size()), wide.data(), static_cast<int>(wide.size()));
        if (written > 0) {
            wide.resize(written);
            return wide;
        }
        return L"";
    }
}

CSapiEngine::CSapiEngine()
{
    memset(&m_audioFormat, 0, sizeof(m_audioFormat));
}

CSapiEngine::~CSapiEngine()
{
#if defined(_DEBUG)
    const ULONGLONG destructionStart = GetTickCount64();
    CoreLog(L"[ThreadTrace] engine_destructor_begin tick=%llu.", destructionStart);
    CoreLog(L"[ThreadTrace] engine_destructor_speak_mutex_wait_begin tick=%llu.", GetTickCount64());
#endif
    std::lock_guard<std::mutex> speakLock(m_speakMutex);
#if defined(_DEBUG)
    CoreLog(L"[ThreadTrace] engine_destructor_speak_mutex_acquired tick=%llu wait_ms=%llu.",
        GetTickCount64(), GetTickCount64() - destructionStart);
#endif
    {
        std::lock_guard<std::mutex> lock(m_siteMutex);
        m_cpSite = nullptr;
    }

    std::lock_guard<std::mutex> sessionLock(m_sessionMutex);
    m_pWorker.reset();

    m_pClient.reset();
#if defined(_DEBUG)
    CoreLog(L"[ThreadTrace] engine_destructor_end tick=%llu duration_ms=%llu.",
        GetTickCount64(), GetTickCount64() - destructionStart);
#endif
}

IFACEMETHODIMP CSapiEngine::SetObjectToken(ISpObjectToken* pToken) noexcept try
{
    CoreLog(L"[CoreEngine] SetObjectToken called.");
    if (!pToken) return E_POINTER;
#if defined(_DEBUG)
    const ULONGLONG tokenMutexWaitStart = GetTickCount64();
    CoreLog(L"[ThreadTrace] set_object_token_speak_mutex_wait_begin tick=%llu.", tokenMutexWaitStart);
#endif
    std::lock_guard<std::mutex> speakLock(m_speakMutex);
#if defined(_DEBUG)
    CoreLog(L"[ThreadTrace] set_object_token_speak_mutex_acquired tick=%llu wait_ms=%llu.",
        GetTickCount64(), GetTickCount64() - tokenMutexWaitStart);
#endif
    {
        std::lock_guard<std::mutex> tokenLock(m_tokenMutex);
        m_cpToken.copy_from(pToken);
    }
    return LoadProviderFromToken(pToken);
}
catch (const std::exception& e) { CoreLog(L"[CoreEngine] SetObjectToken exception: %hs", e.what()); return winrt::to_hresult(); }
catch (...) { CoreLog(L"[CoreEngine] SetObjectToken unknown exception."); return winrt::to_hresult(); }

IFACEMETHODIMP CSapiEngine::GetObjectToken(ISpObjectToken** ppToken) noexcept try
{
    CoreLog(L"[CoreEngine] GetObjectToken called.");
    if (!ppToken) return E_POINTER;
#if defined(_DEBUG)
    const ULONGLONG tokenMutexWaitStart = GetTickCount64();
    CoreLog(L"[ThreadTrace] get_object_token_token_mutex_wait_begin tick=%llu.", tokenMutexWaitStart);
#endif
    std::lock_guard<std::mutex> tokenLock(m_tokenMutex);
#if defined(_DEBUG)
    CoreLog(L"[ThreadTrace] get_object_token_token_mutex_acquired tick=%llu wait_ms=%llu.",
        GetTickCount64(), GetTickCount64() - tokenMutexWaitStart);
#endif
    m_cpToken.copy_to(ppToken);
    return S_OK;
}
catch (const std::exception& e) { CoreLog(L"[CoreEngine] GetObjectToken exception: %hs", e.what()); return winrt::to_hresult(); }
catch (...) { CoreLog(L"[CoreEngine] GetObjectToken unknown exception."); return winrt::to_hresult(); }

IFACEMETHODIMP CSapiEngine::GetOutputFormat(const GUID* pTargetFmtId,
                                            const WAVEFORMATEX* pTargetWaveFormatEx,
                                            GUID* pOutputFormatId,
                                            WAVEFORMATEX** ppCoMemOutputWaveFormatEx) noexcept
{
    CoreLog(L"[CoreEngine] GetOutputFormat called.");
    if (pOutputFormatId) *pOutputFormatId = GUID_NULL;
    if (ppCoMemOutputWaveFormatEx) *ppCoMemOutputWaveFormatEx = nullptr;

#if defined(_DEBUG)
    const ULONGLONG sessionMutexWaitStart = GetTickCount64();
    CoreLog(L"[ThreadTrace] get_output_format_session_mutex_wait_begin tick=%llu.", sessionMutexWaitStart);
#endif
    std::lock_guard<std::mutex> sessionLock(m_sessionMutex);
#if defined(_DEBUG)
    CoreLog(L"[ThreadTrace] get_output_format_session_mutex_acquired tick=%llu wait_ms=%llu.",
        GetTickCount64(), GetTickCount64() - sessionMutexWaitStart);
#endif
    if (!m_hasOutputFormat)
    {
        CoreLog(L"[CoreEngine] GetOutputFormat failed: Client not initialized.");
        return SPERR_UNINITIALIZED;
    }

    if (pOutputFormatId) *pOutputFormatId = SPDFID_WaveFormatEx;
    
    if (ppCoMemOutputWaveFormatEx)
    {
        *ppCoMemOutputWaveFormatEx = (WAVEFORMATEX*)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
        if (!*ppCoMemOutputWaveFormatEx) return E_OUTOFMEMORY;
        **ppCoMemOutputWaveFormatEx = m_audioFormat;
    }
    return S_OK;
}

IFACEMETHODIMP CSapiEngine::Speak(DWORD /*dwSpeakFlags*/,
                                  REFGUID rguidFormatId,
                                  const WAVEFORMATEX* pWaveFormatEx,
                                  const SPVTEXTFRAG* pTextFragList,
                                  ISpTTSEngineSite* pOutputSite) noexcept try
{
    CoreLog(L"[CoreEngine] Speak called.");
    if (!pOutputSite || !pTextFragList) return E_INVALIDARG;

#if defined(_DEBUG)
    const ULONGLONG speakMutexWaitStart = GetTickCount64();
#endif
    std::lock_guard<std::mutex> speakLock(m_speakMutex);
#if defined(_DEBUG)
    CoreLog(L"[CancelTrace] speak_mutex_acquired tick=%llu wait_ms=%llu.",
        GetTickCount64(), GetTickCount64() - speakMutexWaitStart);
#endif

    {
        std::lock_guard<std::mutex> lock(m_siteMutex);
        m_cpSite.copy_from(pOutputSite);
    }

    SpeechWorker* worker = nullptr;
    PipeClient* client = nullptr;
    std::wstring voiceId;
    {
        std::lock_guard<std::mutex> sessionLock(m_sessionMutex);
        if (!m_pClient || !m_pWorker || m_pWorker->IsFaulted())
        {
#if defined(_DEBUG)
            const ULONGLONG sessionRecoveryStart = GetTickCount64();
            CoreLog(L"[CancelTrace] session_recovery_begin tick=%llu.", sessionRecoveryStart);
#endif
            RetireFaultedSessionLocked();
            if (FAILED(CreateProviderSessionLocked()))
            {
                return E_FAIL;
            }
#if defined(_DEBUG)
            CoreLog(L"[CancelTrace] session_recovery_end tick=%llu duration_ms=%llu.",
                GetTickCount64(), GetTickCount64() - sessionRecoveryStart);
#endif
        }

        worker = m_pWorker.get();
        client = m_pClient.get();
        voiceId = m_voiceId;
    }

    uint64_t speakId = ++m_speakIdCounter;
    nlohmann::json speakReq = {
        {"command", "sapi_speak"},
        {"speak_id", speakId}
    };
    if (!voiceId.empty())
    {
        std::string utf8VoiceId = WideToUtf8(voiceId.c_str(), voiceId.size());
        if (!utf8VoiceId.empty())
        {
            speakReq["voice_id"] = utf8VoiceId;
        }
    }

    nlohmann::json fragments = nlohmann::json::array();
    const SPVTEXTFRAG* pFrag = pTextFragList;
    while (pFrag)
    {
        nlohmann::json fragJson;
        
        if (pFrag->State.eAction == SPVA_Bookmark)
        {
            if (pFrag->pTextStart && pFrag->ulTextLen > 0)
            {
                std::string textStr = WideToUtf8(pFrag->pTextStart, pFrag->ulTextLen);
                if (!textStr.empty())
                {
                    fragJson["bookmark"] = textStr;
                }
            }
        }
        else if (pFrag->State.eAction == SPVA_Silence)
        {
            fragJson["silence_ms"] = pFrag->State.SilenceMSecs;
        }
        else // SPVA_Speak, SPVA_Pronounce, etc.
        {
            if (pFrag->pTextStart && pFrag->ulTextLen > 0)
            {
                std::string textStr = WideToUtf8(pFrag->pTextStart, pFrag->ulTextLen);
                if (!textStr.empty())
                {
                    fragJson["text"] = textStr;
                    fragJson["source_offset"] = pFrag->ulTextSrcOffset;
                }
            }
            fragJson["volume"] = pFrag->State.Volume;
            fragJson["pitch"] = pFrag->State.PitchAdj.MiddleAdj;
            fragJson["rate"] = pFrag->State.RateAdj;
        }
        
        fragments.push_back(fragJson);
        pFrag = pFrag->pNext;
    }

    speakReq["fragments"] = fragments;

    if (!worker->Start(pOutputSite, speakId))
    {
        return E_FAIL;
    }

#if defined(_DEBUG)
    const ULONGLONG speakDispatchStart = GetTickCount64();
    CoreLog(L"[CancelTrace] speak_id=%llu sapi_speak_send_begin tick=%llu.", speakId, speakDispatchStart);
#endif
    HRESULT hr = client->SendControlMessage(speakReq);
#if defined(_DEBUG)
    CoreLog(L"[CancelTrace] speak_id=%llu sapi_speak_send_end tick=%llu duration_ms=%llu hr=0x%08x.",
        speakId, GetTickCount64(), GetTickCount64() - speakDispatchStart, hr);
#endif
    if (FAILED(hr))
    {
        worker->EnterFaultedState();
        return hr;
    }

    // A transport failure leaves the session Faulted with outstanding pipe I/O cancelled.
    // The next Speak owns worker/client retirement and replacement under the session lifecycle lock.
    hr = worker->WaitUntilFinished(pOutputSite);
#if defined(_DEBUG)
    CoreLog(L"[CancelTrace] speak_id=%llu speak_return tick=%llu hr=0x%08x.",
        speakId, GetTickCount64(), hr);
#endif
    return hr;
}
catch (const std::exception& e) { CoreLog(L"[CoreEngine] Speak exception: %hs", e.what()); return winrt::to_hresult(); }
catch (...) { CoreLog(L"[CoreEngine] Speak unknown exception."); return winrt::to_hresult(); }

bool CSapiEngine::OnAudioData(const uint8_t* pAudioBytes, uint32_t byteCount)
{
    winrt::com_ptr<ISpTTSEngineSite> site;
    {
        std::lock_guard<std::mutex> lock(m_siteMutex);
        if (!m_cpSite) return false;
        site = m_cpSite;
    }

    ULONG bytesWritten = 0;
#if defined(_DEBUG)
    const uint64_t speakId = m_speakIdCounter.load(std::memory_order_acquire);
    const ULONGLONG writeStartTick = GetTickCount64();
    CoreLog(L"[ThreadTrace] speak_id=%llu sapi_write_begin tick=%llu requested=%lu.",
        speakId, writeStartTick, byteCount);
#endif
    HRESULT hr = site->Write(pAudioBytes, byteCount, &bytesWritten);
#if defined(_DEBUG)
    CoreLog(L"[ThreadTrace] speak_id=%llu sapi_write_end tick=%llu duration_ms=%llu hr=0x%08x requested=%lu written=%lu.",
        speakId, GetTickCount64(), GetTickCount64() - writeStartTick, hr, byteCount, bytesWritten);
#endif
    return SUCCEEDED(hr) && (bytesWritten == byteCount);
}

#if defined(_DEBUG)
void CSapiEngine::FailNextCancellationControlSendForTest()
{
    std::lock_guard<std::mutex> sessionLock(m_sessionMutex);
    if (m_pClient)
    {
        m_pClient->FailNextCancellationMessageForTest();
    }
}

void CSapiEngine::FailNextSpeakControlSendForTest()
{
    std::lock_guard<std::mutex> sessionLock(m_sessionMutex);
    if (m_pClient)
    {
        m_pClient->FailNextSpeakMessageForTest();
    }
}
#endif

uint64_t CSapiEngine::AudioOffsetMsToBytes(uint32_t audioMs) const
{
    if (m_audioFormat.nSamplesPerSec == 0 || m_audioFormat.nBlockAlign == 0)
    {
        return 0;
    }

    const uint64_t frames = (static_cast<uint64_t>(audioMs) * m_audioFormat.nSamplesPerSec) / 1000;
    return frames * m_audioFormat.nBlockAlign;
}



void CSapiEngine::OnSpeechEvent(const nlohmann::json& eventJson) try
{
    winrt::com_ptr<ISpTTSEngineSite> site;
    {
        std::lock_guard<std::mutex> lock(m_siteMutex);
        if (!m_cpSite) return;
        site = m_cpSite;
    }

    if (!eventJson.contains("event") || !eventJson["event"].is_string()) return;
    const std::string_view eventStr = eventJson["event"].get<std::string_view>();
    
    if (eventJson.contains("speak_id"))
    {
        uint64_t eventSpeakId = 0;
        if (!TryGetJsonUnsignedInteger(eventJson["speak_id"], eventSpeakId))
        {
            return;
        }
        if (eventSpeakId != m_speakIdCounter.load())
        {
            return; // Drop stale event
        }
    }

    if (eventStr == "word_boundary")
    {
        uint32_t audioMs = 0;
        uint32_t textOffset = 0;
        uint32_t textLength = 0;
        if (!eventJson.contains("audio_offset_ms") || !TryGetJsonUnsignedInteger(eventJson["audio_offset_ms"], audioMs) ||
            !eventJson.contains("text_offset") || !TryGetJsonUnsignedInteger(eventJson["text_offset"], textOffset) ||
            !eventJson.contains("text_length") || !TryGetJsonUnsignedInteger(eventJson["text_length"], textLength))
        {
            return;
        }

        SPEVENT spEvent = {};
        spEvent.eEventId = SPEI_WORD_BOUNDARY;
        spEvent.elParamType = SPET_LPARAM_IS_UNDEFINED;
        spEvent.ullAudioStreamOffset = AudioOffsetMsToBytes(audioMs);

        spEvent.wParam = static_cast<WPARAM>(textLength);
        spEvent.lParam = static_cast<LPARAM>(textOffset);
        
        site->AddEvents(&spEvent, 1);
    }
    else if (eventStr == "sentence_boundary")
    {
        uint32_t audioMs = 0;
        uint32_t textOffset = 0;
        uint32_t textLength = 0;
        if (!eventJson.contains("audio_offset_ms") || !TryGetJsonUnsignedInteger(eventJson["audio_offset_ms"], audioMs) ||
            !eventJson.contains("text_offset") || !TryGetJsonUnsignedInteger(eventJson["text_offset"], textOffset) ||
            !eventJson.contains("text_length") || !TryGetJsonUnsignedInteger(eventJson["text_length"], textLength))
        {
            return;
        }

        SPEVENT spEvent = {};
        spEvent.eEventId = SPEI_SENTENCE_BOUNDARY;
        spEvent.elParamType = SPET_LPARAM_IS_UNDEFINED;
        spEvent.ullAudioStreamOffset = AudioOffsetMsToBytes(audioMs);

        spEvent.wParam = static_cast<WPARAM>(textLength);
        spEvent.lParam = static_cast<LPARAM>(textOffset);
        
        site->AddEvents(&spEvent, 1);
    }
    else if (eventStr == "bookmark_reached")
    {
        uint32_t audioMs = 0;
        if (!eventJson.contains("audio_offset_ms") || !TryGetJsonUnsignedInteger(eventJson["audio_offset_ms"], audioMs))
        {
            return;
        }

        SPEVENT spEvent = {};
        spEvent.eEventId = SPEI_TTS_BOOKMARK;
        spEvent.ullAudioStreamOffset = AudioOffsetMsToBytes(audioMs);

        if (eventJson.contains("bookmark_name") && eventJson["bookmark_name"].is_string())
        {
            auto bNameStr = eventJson["bookmark_name"].get<std::string>();
            if (!bNameStr.empty())
            {
                wchar_t* pStr = (wchar_t*)CoTaskMemAlloc((bNameStr.size() + 1) * sizeof(wchar_t));
                if (pStr)
                {
                    int written = MultiByteToWideChar(CP_UTF8, 0, bNameStr.c_str(), static_cast<int>(bNameStr.size()), pStr, static_cast<int>(bNameStr.size()));
                    if (written >= 0)
                    {
                        pStr[written] = L'\0';
                        spEvent.elParamType = SPET_LPARAM_IS_STRING;
                        spEvent.wParam = static_cast<WPARAM>(_wtol(pStr));
                        spEvent.lParam = (LPARAM)pStr;
                    }
                    else
                    {
                        CoTaskMemFree(pStr);
                        spEvent.elParamType = SPET_LPARAM_IS_UNDEFINED;
                    }
                }
                else
                {
                    spEvent.elParamType = SPET_LPARAM_IS_UNDEFINED;
                }
            }
            else
            {
                spEvent.elParamType = SPET_LPARAM_IS_UNDEFINED;
            }
        }
        else
        {
            spEvent.elParamType = SPET_LPARAM_IS_UNDEFINED;
        }
        
        site->AddEvents(&spEvent, 1);
    }
    else if (eventStr == "log")
    {
        auto getUtf16String = [](const nlohmann::json& j, const char* key, const wchar_t* defaultVal) -> std::wstring {
            if (!j.contains(key) || !j[key].is_string()) return defaultVal;
            std::string s = j[key].get<std::string>();
            std::wstring w = Utf8ToWide(s);
            return w.empty() ? defaultVal : w;
        };

        std::wstring msg = getUtf16String(eventJson, "message", L"Unknown log");
        std::wstring severity = getUtf16String(eventJson, "severity", L"error");
        std::wstring friendly = getUtf16String(eventJson, "friendly_text", L"");
        
        CoreLog(L"[CoreEngine] Provider error (%s): %s %s", severity.c_str(), msg.c_str(), friendly.c_str());
    }
}
catch (...)
{
    CoreLog(L"[CoreEngine] Exception in OnSpeechEvent.");
}

HRESULT CSapiEngine::LoadProviderFromToken(ISpObjectToken* pToken)
{
    wil::unique_cotaskmem_string pszExe;
    HRESULT hr = pToken->GetStringValue(L"ProviderExecutablePath", &pszExe);
    if (FAILED(hr)) return hr;

    wil::unique_cotaskmem_string pszPipe;
    HRESULT hrPipe = pToken->GetStringValue(L"ProviderPipeName", &pszPipe);
    if (FAILED(hrPipe)) return hrPipe;

    wil::unique_cotaskmem_string pszVoice;
    std::wstring voiceId;
    if (SUCCEEDED(pToken->GetStringValue(L"VoiceId", &pszVoice)))
    {
        voiceId = pszVoice.get();
    }

    std::lock_guard<std::mutex> sessionLock(m_sessionMutex);
    if (m_pClient || m_pWorker)
    {
        return E_UNEXPECTED;
    }

    m_providerExecutablePath = pszExe.get();
    m_providerPipeName = pszPipe.get();
    m_voiceId = std::move(voiceId);
    return CreateProviderSessionLocked();
}

HRESULT CSapiEngine::CreateProviderSessionLocked()
{
    if (m_providerExecutablePath.empty() || m_providerPipeName.empty() || m_pClient || m_pWorker)
    {
        return E_FAIL;
    }

    try
    {
        auto candidateClient = std::make_unique<PipeClient>();
        if (FAILED(candidateClient->Connect(m_providerPipeName, m_providerExecutablePath)))
        {
            return E_FAIL;
        }

        nlohmann::json infoRequest = {
            {"command", "info"}
        };
        if (FAILED(candidateClient->SendControlMessage(infoRequest)))
        {
            return E_FAIL;
        }

        nlohmann::json infoResponse;
        if (FAILED(candidateClient->ReadControlMessage(
                infoResponse, PipeClient::ControlOperationTimeoutMs)) ||
            !infoResponse.contains("response") ||
            !infoResponse["response"].is_string() ||
            infoResponse["response"].get<std::string>() != "info" ||
            !infoResponse.contains("audio_format") ||
            !infoResponse["audio_format"].is_object())
        {
            return E_FAIL;
        }

        WAVEFORMATEX candidateFormat = {};
        if (!AudioFormatUtils::TryParseAudioFormatJson(infoResponse["audio_format"], candidateFormat))
        {
            return E_FAIL;
        }

        if (m_hasOutputFormat &&
            (candidateFormat.nSamplesPerSec != m_audioFormat.nSamplesPerSec ||
             candidateFormat.wBitsPerSample != m_audioFormat.wBitsPerSample ||
             candidateFormat.nChannels != m_audioFormat.nChannels ||
             candidateFormat.nBlockAlign != m_audioFormat.nBlockAlign))
        {
            CoreLog(L"[CoreEngine] Provider reconnect returned a different PCM format.");
            return E_FAIL;
        }

        auto candidateWorker = std::make_unique<SpeechWorker>(this, candidateClient.get(), candidateFormat.nBlockAlign);
        m_pClient = std::move(candidateClient);
        m_pWorker = std::move(candidateWorker);
        if (!m_hasOutputFormat)
        {
            m_audioFormat = candidateFormat;
            m_hasOutputFormat = true;
        }
        return S_OK;
    }
    catch (const winrt::hresult_error& error)
    {
        CoreLog(L"[CoreEngine] Provider session initialization failed: 0x%08x", error.code().value);
    }
    catch (const std::exception& error)
    {
        CoreLog(L"[CoreEngine] Provider session initialization failed: %hs", error.what());
    }
    catch (...)
    {
        CoreLog(L"[CoreEngine] Provider session initialization failed.");
    }

    return E_FAIL;
}

void CSapiEngine::RetireFaultedSessionLocked()
{
    m_pWorker.reset();
    m_pClient.reset();
}
