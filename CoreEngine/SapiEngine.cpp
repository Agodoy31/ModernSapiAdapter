#include "pch.h"
#include "SapiEngine.h"

CSapiEngine::CSapiEngine()
{
    memset(&m_audioFormat, 0, sizeof(m_audioFormat));
}

CSapiEngine::~CSapiEngine()
{
    if (m_pWorker)
    {
        m_pWorker->Stop();
        {
            std::lock_guard<std::mutex> lock(m_siteMutex);
            m_cpSite = nullptr;
        }
        m_pWorker->WaitUntilFinished();
    }
    m_pWorker.reset();
    
    if (m_pClient)
    {
        // Send explicit shutdown command to external provider before disconnecting pipe client
        using namespace winrt::Windows::Data::Json;
        JsonObject shutdownReq;
        shutdownReq.SetNamedValue(L"command", JsonValue::CreateStringValue(L"shutdown"));
        m_pClient->SendControlMessage(shutdownReq);
    }
    m_pClient.reset();
}

IFACEMETHODIMP CSapiEngine::SetObjectToken(ISpObjectToken* pToken) noexcept try
{
    CoreLog(L"[CoreEngine] SetObjectToken called.");
    if (!pToken) return E_POINTER;
    m_cpToken.copy_from(pToken);
    return LoadProviderFromToken(pToken);
}
catch (const std::exception& e) { CoreLog(L"[CoreEngine] SetObjectToken exception: %hs", e.what()); return winrt::to_hresult(); }
catch (...) { CoreLog(L"[CoreEngine] SetObjectToken unknown exception."); return winrt::to_hresult(); }

IFACEMETHODIMP CSapiEngine::GetObjectToken(ISpObjectToken** ppToken) noexcept
{
    if (!ppToken) return E_POINTER;
    m_cpToken.copy_to(ppToken);
    return S_OK;
}

IFACEMETHODIMP CSapiEngine::GetOutputFormat(const GUID* pTargetFmtId,
                                            const WAVEFORMATEX* pTargetWaveFormatEx,
                                            GUID* pOutputFormatId,
                                            WAVEFORMATEX** ppCoMemOutputWaveFormatEx) noexcept
{
    CoreLog(L"[CoreEngine] GetOutputFormat called.");
    if (!m_pClient)
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

IFACEMETHODIMP CSapiEngine::Speak(DWORD dwSpeakFlags,
                                  REFGUID rguidFormatId,
                                  const WAVEFORMATEX* pWaveFormatEx,
                                  const SPVTEXTFRAG* pTextFragList,
                                  ISpTTSEngineSite* pOutputSite) noexcept try
{
    CoreLog(L"[CoreEngine] Speak called.");
    if (!pOutputSite || !pTextFragList) return E_INVALIDARG;

    {
        std::lock_guard<std::mutex> lock(m_siteMutex);
        m_cpSite.copy_from(pOutputSite);
    }

    if (!m_pClient) return E_FAIL;

    using namespace winrt::Windows::Data::Json;
    uint64_t speakId = ++m_speakIdCounter;
    JsonObject speakReq;
    speakReq.SetNamedValue(L"command", JsonValue::CreateStringValue(L"sapi_speak"));
    speakReq.SetNamedValue(L"speak_id", JsonValue::CreateNumberValue(static_cast<double>(speakId)));
    if (!m_voiceId.empty())
    {
        speakReq.SetNamedValue(L"voice_id", JsonValue::CreateStringValue(m_voiceId));
    }

    JsonArray fragments;
    const SPVTEXTFRAG* pFrag = pTextFragList;
    while (pFrag)
    {
        JsonObject fragJson;
        
        if (pFrag->State.eAction == SPVA_Bookmark)
        {
            if (pFrag->pTextStart && pFrag->ulTextLen > 0)
            {
                std::wstring textStr((const wchar_t*)pFrag->pTextStart, pFrag->ulTextLen);
                fragJson.SetNamedValue(L"bookmark", JsonValue::CreateStringValue(textStr));
            }
        }
        else if (pFrag->State.eAction == SPVA_Silence)
        {
            fragJson.SetNamedValue(L"silence_ms", JsonValue::CreateNumberValue(pFrag->State.SilenceMSecs));
        }
        else // SPVA_Speak, SPVA_Pronounce, etc.
        {
            if (pFrag->pTextStart && pFrag->ulTextLen > 0)
            {
                std::wstring textStr((const wchar_t*)pFrag->pTextStart, pFrag->ulTextLen);
                fragJson.SetNamedValue(L"text", JsonValue::CreateStringValue(textStr));
            }
            fragJson.SetNamedValue(L"volume", JsonValue::CreateNumberValue(pFrag->State.Volume));
            fragJson.SetNamedValue(L"pitch", JsonValue::CreateNumberValue(pFrag->State.PitchAdj.MiddleAdj));
            fragJson.SetNamedValue(L"rate", JsonValue::CreateNumberValue(pFrag->State.RateAdj));
        }
        
        fragments.Append(fragJson);
        pFrag = pFrag->pNext;
    }

    speakReq.SetNamedValue(L"fragments", fragments);

    m_pWorker->Start(pOutputSite, speakId);

    HRESULT hr = m_pClient->SendControlMessage(speakReq);
    if (FAILED(hr))
    {
        m_pWorker->Stop();
        return hr;
    }

    if ((dwSpeakFlags & SPF_ASYNC) == 0)
    {
        m_pWorker->WaitUntilFinished();
    }

    return S_OK;
}
catch (const std::exception& e) { CoreLog(L"[CoreEngine] Speak exception: %hs", e.what()); return winrt::to_hresult(); }
catch (...) { CoreLog(L"[CoreEngine] Speak unknown exception."); return winrt::to_hresult(); }

bool CSapiEngine::OnAudioData(const uint8_t* pAudioBytes, uint32_t byteCount)
{
    std::lock_guard<std::mutex> lock(m_siteMutex);
    if (!m_cpSite) return false;

    ULONG bytesWritten = 0;
    HRESULT hr = m_cpSite->Write(pAudioBytes, byteCount, &bytesWritten);
    return SUCCEEDED(hr) && (bytesWritten == byteCount);
}

void CSapiEngine::OnSpeechEvent(const winrt::Windows::Data::Json::JsonObject& eventJson) try
{
    std::lock_guard<std::mutex> lock(m_siteMutex);
    if (!m_cpSite) return;

    if (!eventJson.HasKey(L"event")) return;
    if (eventJson.GetNamedValue(L"event").ValueType() != winrt::Windows::Data::Json::JsonValueType::String) return;
    auto eventStr = eventJson.GetNamedString(L"event");
    
    if (eventJson.HasKey(L"speak_id") && eventJson.GetNamedValue(L"speak_id").ValueType() == winrt::Windows::Data::Json::JsonValueType::Number)
    {
        uint64_t eventSpeakId = static_cast<uint64_t>(eventJson.GetNamedNumber(L"speak_id"));
        if (eventSpeakId != m_speakIdCounter.load())
        {
            return; // Drop stale event
        }
    }

    if (eventStr == L"word_boundary")
    {
        SPEVENT spEvent = {};
        spEvent.eEventId = SPEI_WORD_BOUNDARY;
        spEvent.elParamType = SPET_LPARAM_IS_UNDEFINED;
        
        uint32_t audioMs = eventJson.HasKey(L"audio_offset_ms") ? static_cast<uint32_t>(eventJson.GetNamedNumber(L"audio_offset_ms")) : 0;
        uint32_t bytesOffset = (m_audioFormat.nAvgBytesPerSec > 0) ? (audioMs * m_audioFormat.nAvgBytesPerSec) / 1000 : 0;
        spEvent.ullAudioStreamOffset = bytesOffset;

        uint32_t textOffset = eventJson.HasKey(L"text_offset") ? static_cast<uint32_t>(eventJson.GetNamedNumber(L"text_offset")) : 0;
        uint32_t textLength = eventJson.HasKey(L"text_length") ? static_cast<uint32_t>(eventJson.GetNamedNumber(L"text_length")) : 0;
        spEvent.lParam = textOffset;
        spEvent.wParam = textLength;
        
        m_cpSite->AddEvents(&spEvent, 1);
    }
    else if (eventStr == L"sentence_boundary")
    {
        SPEVENT spEvent = {};
        spEvent.eEventId = SPEI_SENTENCE_BOUNDARY;
        spEvent.elParamType = SPET_LPARAM_IS_UNDEFINED;
        
        uint32_t audioMs = eventJson.HasKey(L"audio_offset_ms") ? static_cast<uint32_t>(eventJson.GetNamedNumber(L"audio_offset_ms")) : 0;
        uint32_t bytesOffset = (m_audioFormat.nAvgBytesPerSec > 0) ? (audioMs * m_audioFormat.nAvgBytesPerSec) / 1000 : 0;
        spEvent.ullAudioStreamOffset = bytesOffset;

        uint32_t textOffset = eventJson.HasKey(L"text_offset") ? static_cast<uint32_t>(eventJson.GetNamedNumber(L"text_offset")) : 0;
        uint32_t textLength = eventJson.HasKey(L"text_length") ? static_cast<uint32_t>(eventJson.GetNamedNumber(L"text_length")) : 0;
        spEvent.lParam = textOffset;
        spEvent.wParam = textLength;
        
        m_cpSite->AddEvents(&spEvent, 1);
    }
    else if (eventStr == L"bookmark_reached")
    {
        SPEVENT spEvent = {};
        spEvent.eEventId = SPEI_TTS_BOOKMARK;
        
        uint32_t audioMs = eventJson.HasKey(L"audio_offset_ms") ? static_cast<uint32_t>(eventJson.GetNamedNumber(L"audio_offset_ms")) : 0;
        uint32_t bytesOffset = (m_audioFormat.nAvgBytesPerSec > 0) ? (audioMs * m_audioFormat.nAvgBytesPerSec) / 1000 : 0;
        spEvent.ullAudioStreamOffset = bytesOffset;

        if (eventJson.HasKey(L"bookmark_name"))
        {
            auto bName = eventJson.GetNamedString(L"bookmark_name");
            size_t numChars = bName.size() + 1;
            wchar_t* pStr = (wchar_t*)CoTaskMemAlloc(numChars * sizeof(wchar_t));
            if (pStr)
            {
                wcscpy_s(pStr, numChars, bName.c_str());
                spEvent.elParamType = SPET_LPARAM_IS_STRING;
                spEvent.lParam = (LPARAM)pStr;
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
        
        m_cpSite->AddEvents(&spEvent, 1);
    }
    else if (eventStr == L"error")
    {
        std::wstring msg = eventJson.HasKey(L"message") ? std::wstring(eventJson.GetNamedString(L"message").c_str()) : L"Unknown error";
        std::wstring severity = eventJson.HasKey(L"severity") ? std::wstring(eventJson.GetNamedString(L"severity").c_str()) : L"error";
        std::wstring friendly = eventJson.HasKey(L"friendly_text") ? std::wstring(eventJson.GetNamedString(L"friendly_text").c_str()) : L"";
        
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
    if (SUCCEEDED(pToken->GetStringValue(L"VoiceId", &pszVoice)))
    {
        m_voiceId = pszVoice.get();
    }

    std::wstring exePath(pszExe.get());
    std::wstring pipeName(pszPipe.get());

    m_pClient = std::make_shared<PipeClient>();
    hr = m_pClient->Connect(pipeName, exePath);
    if (FAILED(hr)) return hr;

    using namespace winrt::Windows::Data::Json;
    JsonObject infoReq;
    infoReq.SetNamedValue(L"command", JsonValue::CreateStringValue(L"info"));
    
    if (SUCCEEDED(m_pClient->SendControlMessage(infoReq)))
    {
        JsonObject infoRes = nullptr;
        if (SUCCEEDED(m_pClient->ReadControlMessage(infoRes)))
        {
            std::wstring pName = infoRes.HasKey(L"provider_name") ? std::wstring(infoRes.GetNamedString(L"provider_name").c_str()) : L"Unknown";
            std::wstring pVer = infoRes.HasKey(L"version") ? std::wstring(infoRes.GetNamedString(L"version").c_str()) : L"Unknown";
            CoreLog(L"[CoreEngine] Connected to provider: %s v%s", pName.c_str(), pVer.c_str());

            if (infoRes.HasKey(L"audio_format"))
            {
                auto fmtJson = infoRes.GetNamedObject(L"audio_format");
                m_audioFormat.wFormatTag = WAVE_FORMAT_PCM;
                m_audioFormat.nSamplesPerSec = static_cast<DWORD>(fmtJson.GetNamedNumber(L"sample_rate"));
                m_audioFormat.wBitsPerSample = static_cast<WORD>(fmtJson.GetNamedNumber(L"bits_per_sample"));
                m_audioFormat.nChannels = static_cast<WORD>(fmtJson.GetNamedNumber(L"channels"));
                m_audioFormat.nBlockAlign = (m_audioFormat.nChannels * m_audioFormat.wBitsPerSample) / 8;
                m_audioFormat.nAvgBytesPerSec = m_audioFormat.nSamplesPerSec * m_audioFormat.nBlockAlign;
                m_audioFormat.cbSize = 0;
            }
        }
    }

    m_pWorker = std::make_unique<SpeechWorker>(this, m_pClient);

    return S_OK;
}
