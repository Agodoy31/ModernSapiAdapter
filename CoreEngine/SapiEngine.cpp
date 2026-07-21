#include "pch.h"
#include "SapiEngine.h"
#include "XmlToSsmlMapper.h"

CSapiEngine::CSapiEngine()
{
    m_pWrapper = std::make_unique<ProviderWrapper>();
    m_pWorker = std::make_unique<SpeechWorker>(this, m_pWrapper.get());
}

CSapiEngine::~CSapiEngine()
{
    // TEARDOWN GUARDRAIL: We must guarantee SpeechWorker thread is completely joined
    // BEFORE ProviderWrapper destructs (and drops its wil::unique_hmodule handle).
    if (m_pWorker)
    {
        m_pWorker->Stop();
        m_pWorker->WaitUntilFinished();
    }
    
    // Explicitly reset the worker so it's deleted before wrapper.
    m_pWorker.reset();
    m_pWrapper.reset();
}

IFACEMETHODIMP CSapiEngine::SetObjectToken(ISpObjectToken* pToken) noexcept try
{
    if (!pToken) return E_POINTER;
    m_cpToken.copy_from(pToken);

    return LoadProviderFromToken(pToken);
}
catch (...) { return winrt::to_hresult(); }

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
    if (!m_pWrapper || !m_pWrapper->IsLoaded())
    {
        return SPERR_UNINITIALIZED;
    }

    ProviderAudioFormat format{};
    if (!m_pWrapper->GetAudioFormat(&format))
    {
        return E_FAIL;
    }

    if (pOutputFormatId) *pOutputFormatId = SPDFID_WaveFormatEx;
    
    if (ppCoMemOutputWaveFormatEx)
    {
        if (pTargetWaveFormatEx)
        {
            *ppCoMemOutputWaveFormatEx = (WAVEFORMATEX*)CoTaskMemAlloc(sizeof(WAVEFORMATEX) + pTargetWaveFormatEx->cbSize);
            if (!*ppCoMemOutputWaveFormatEx) return E_OUTOFMEMORY;
            memcpy(*ppCoMemOutputWaveFormatEx, pTargetWaveFormatEx, sizeof(WAVEFORMATEX) + pTargetWaveFormatEx->cbSize);
        }
        else
        {
            *ppCoMemOutputWaveFormatEx = (WAVEFORMATEX*)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
            if (!*ppCoMemOutputWaveFormatEx) return E_OUTOFMEMORY;
            WAVEFORMATEX* pwf = *ppCoMemOutputWaveFormatEx;
            pwf->wFormatTag = WAVE_FORMAT_PCM;
            pwf->nChannels = format.Channels;
            pwf->nSamplesPerSec = format.SampleRate;
            pwf->wBitsPerSample = format.BitsPerSample;
            pwf->nBlockAlign = (pwf->nChannels * pwf->wBitsPerSample) / 8;
            pwf->nAvgBytesPerSec = pwf->nSamplesPerSec * pwf->nBlockAlign;
            pwf->cbSize = 0;
        }
    }
    return S_OK;
}

IFACEMETHODIMP CSapiEngine::Speak(DWORD dwSpeakFlags,
                                  REFGUID rguidFormatId,
                                  const WAVEFORMATEX* pWaveFormatEx,
                                  const SPVTEXTFRAG* pTextFragList,
                                  ISpTTSEngineSite* pOutputSite) noexcept try
{
    if (!pOutputSite || !pTextFragList) return E_INVALIDARG;

    m_cpSite.copy_from(pOutputSite);

    std::u16string fullText;
    const SPVTEXTFRAG* pFrag = pTextFragList;
    while (pFrag)
    {
        if (pFrag->pTextStart && pFrag->ulTextLen > 0)
        {
            fullText.append((const char16_t*)pFrag->pTextStart, pFrag->ulTextLen);
        }
        pFrag = pFrag->pNext;
    }

    std::u16string ssmlText = XmlToSsmlMapper::Translate(fullText);

    m_pWorker->Start(ssmlText);

    while (!m_pWorker->IsFinished())
    {
        DWORD actions = m_cpSite->GetActions();
        if (actions & SPVES_ABORT)
        {
            m_pWorker->Stop();
            break;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    m_pWorker->WaitUntilFinished();
    m_cpSite = nullptr;
    return S_OK;
}
catch (...) { return winrt::to_hresult(); }

HRESULT CSapiEngine::LoadProviderFromToken(ISpObjectToken* pToken)
{
    wil::com_ptr<ISpDataKey> cpDataKey;
    HRESULT hr = pToken->QueryInterface(IID_PPV_ARGS(&cpDataKey));
    if (FAILED(hr)) return hr;

    wil::unique_cotaskmem_string pDllPath;
    hr = cpDataKey->GetStringValue(L"ProviderDLL", &pDllPath);
    if (FAILED(hr))
    {
        return E_INVALIDARG;
    }

    return m_pWrapper->Load(pDllPath.get());
}

bool CSapiEngine::OnAudioData(const uint8_t* pAudioBytes, uint32_t byteCount)
{
    if (!m_cpSite) return false;

    DWORD actions = m_cpSite->GetActions();
    if (actions & SPVES_ABORT) return false;
    
    ULONG cbWritten = 0;
    HRESULT hr = m_cpSite->Write((const void*)pAudioBytes, byteCount, &cbWritten);
    return SUCCEEDED(hr);
}

void CSapiEngine::OnSpeechEvent(const ProviderSpeechEvent* pEvent)
{
    if (!m_cpSite || !pEvent) return;

    SPEVENT sapiEvent = {};
    sapiEvent.ullAudioStreamOffset = pEvent->AudioByteOffset;
    
    switch (pEvent->EventType)
    {
    case PROVIDER_EVENT_WORD_BOUNDARY:
        sapiEvent.eEventId = SPEI_WORD_BOUNDARY;
        sapiEvent.lParam = pEvent->TextLength;
        sapiEvent.wParam = pEvent->TextOffset;
        break;
    case PROVIDER_EVENT_SENTENCE_BOUNDARY:
        sapiEvent.eEventId = SPEI_SENTENCE_BOUNDARY;
        sapiEvent.lParam = pEvent->TextLength;
        sapiEvent.wParam = pEvent->TextOffset;
        break;
    case PROVIDER_EVENT_BOOKMARK:
        sapiEvent.eEventId = SPEI_TTS_BOOKMARK;
        sapiEvent.lParam = pEvent->TextLength;
        sapiEvent.wParam = pEvent->TextOffset;
        break;
    default:
        // Unsupported event
        return;
    }

    m_cpSite->AddEvents(&sapiEvent, 1);
}
