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

    using namespace winrt::Windows::Data::Json;
    JsonObject req;
    req.SetNamedValue(L"command", JsonValue::CreateStringValue(L"sapi_speak"));
    req.SetNamedValue(L"speak_id", JsonValue::CreateNumberValue(1));
    req.SetNamedValue(L"voice_id", JsonValue::CreateStringValue(m_voiceId));

    JsonArray fragments;
    const SPVTEXTFRAG* pFrag = pTextFragList;
    while (pFrag)
    {
        JsonObject fragJson;
        if (pFrag->pTextStart && pFrag->ulTextLen > 0)
        {
            std::wstring textStr((const wchar_t*)pFrag->pTextStart, pFrag->ulTextLen);
            fragJson.SetNamedValue(L"text", JsonValue::CreateStringValue(textStr));
        }

        if (pFrag->State.eAction == SPVA_Bookmark)
        {
            fragJson.SetNamedValue(L"bookmark", JsonValue::CreateNumberValue(pFrag->ulTextSrcOffset));
        }
        else
        {
            fragJson.SetNamedValue(L"volume", JsonValue::CreateNumberValue(pFrag->State.Volume));
            fragJson.SetNamedValue(L"pitch", JsonValue::CreateNumberValue(pFrag->State.PitchAdj.MiddleAdj));
            fragJson.SetNamedValue(L"rate", JsonValue::CreateNumberValue(pFrag->State.RateAdj));
        }
        fragments.Append(fragJson);
        pFrag = pFrag->pNext;
    }

    req.SetNamedValue(L"fragments", fragments);

    HRESULT hr = m_pClient->SendControlMessage(req);
    if (FAILED(hr)) return hr;

    m_pWorker->Start(pOutputSite);

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

void CSapiEngine::OnSpeechEvent(const winrt::Windows::Data::Json::JsonObject& eventJson)
{
    std::lock_guard<std::mutex> lock(m_siteMutex);
    if (!m_cpSite) return;

    if (!eventJson.HasKey(L"event")) return;
    auto eventStr = eventJson.GetNamedString(L"event");
    if (eventStr == L"word_boundary")
    {
        SPEVENT spEvent = {};
        spEvent.eEventId = SPEI_WORD_BOUNDARY;
        spEvent.elParamType = SPET_LPARAM_IS_UNDEFINED;
        
        uint32_t audioMs = static_cast<uint32_t>(eventJson.GetNamedNumber(L"audio_offset_ms"));
        uint32_t bytesOffset = (audioMs * m_audioFormat.nAvgBytesPerSec) / 1000;
        spEvent.ullAudioStreamOffset = bytesOffset;

        uint32_t textOffset = static_cast<uint32_t>(eventJson.GetNamedNumber(L"text_offset"));
        uint32_t textLength = static_cast<uint32_t>(eventJson.GetNamedNumber(L"text_length"));
        spEvent.lParam = textOffset;
        spEvent.wParam = textLength;
        
        m_cpSite->AddEvents(&spEvent, 1);
    }
    else if (eventStr == L"bookmark_reached")
    {
        SPEVENT spEvent = {};
        spEvent.eEventId = SPEI_TTS_BOOKMARK;
        
        uint32_t audioMs = static_cast<uint32_t>(eventJson.GetNamedNumber(L"audio_offset_ms"));
        uint32_t bytesOffset = (audioMs * m_audioFormat.nAvgBytesPerSec) / 1000;
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
}

HRESULT CSapiEngine::LoadProviderFromToken(ISpObjectToken* pToken)
{
    winrt::com_ptr<ISpDataKey> cpDataKey;
    if (FAILED(pToken->QueryInterface(IID_PPV_ARGS(&cpDataKey)))) return E_FAIL;

    wil::unique_cotaskmem_string pszExe, pszPipe;
    if (FAILED(cpDataKey->GetStringValue(L"ProviderExecutablePath", &pszExe))) return E_FAIL;
    if (FAILED(cpDataKey->GetStringValue(L"ProviderPipeName", &pszPipe))) return E_FAIL;

    wil::unique_cotaskmem_string pszVoiceId;
    if (SUCCEEDED(cpDataKey->GetStringValue(L"VoiceId", &pszVoiceId))) {
        m_voiceId = pszVoiceId.get();
    } else {
        wil::unique_cotaskmem_string pszId;
        pToken->GetId(&pszId);
        m_voiceId = pszId.get();
    }

    m_pClient = std::make_shared<PipeClient>();
    
    std::wstring exePath(pszExe.get());
    std::wstring pipeName(pszPipe.get());

    HRESULT hr = m_pClient->Connect(pipeName, exePath);
    if (FAILED(hr)) return hr;

    m_pWorker = std::make_unique<SpeechWorker>(this, m_pClient);

    using namespace winrt::Windows::Data::Json;
    JsonObject infoReq;
    infoReq.SetNamedValue(L"command", JsonValue::CreateStringValue(L"info"));
    
    if (SUCCEEDED(m_pClient->SendControlMessage(infoReq)))
    {
        JsonObject infoRes = nullptr;
        if (SUCCEEDED(m_pClient->ReadControlMessage(infoRes)))
        {
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

    return S_OK;
}
