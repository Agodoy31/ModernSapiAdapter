#include "pch.h"
#include "SapiEngine.h"

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

    {
        std::lock_guard<std::mutex> lock(m_siteMutex);
        m_cpSite.copy_from(pOutputSite);
    }

    std::vector<char16_t> backingBuffer;
    std::vector<ProviderSpeechFragment> fragments;
    std::vector<uint32_t> fragmentOffsets;
    
    // Pass 1: Count total chars to pre-allocate
    uint32_t totalChars = 0;
    for (const SPVTEXTFRAG* p = pTextFragList; p; p = p->pNext)
    {
        totalChars += p->ulTextLen;
    }
    backingBuffer.reserve(totalChars);

    const SPVTEXTFRAG* pFrag = pTextFragList;
    while (pFrag)
    {
        uint32_t currentOffset = static_cast<uint32_t>(backingBuffer.size());
        if (pFrag->pTextStart && pFrag->ulTextLen > 0)
        {
            backingBuffer.insert(backingBuffer.end(), (const char16_t*)pFrag->pTextStart, (const char16_t*)pFrag->pTextStart + pFrag->ulTextLen);
        }
        
        ProviderSpeechFragment fragment{};
        fragment.TextLength = pFrag->ulTextLen;
        fragment.OriginalOffset = pFrag->ulTextSrcOffset;
        
        if (pFrag->State.eAction == SPVA_Speak) fragment.Action = PROVIDER_ACTION_SPEAK;
        else if (pFrag->State.eAction == SPVA_Bookmark) fragment.Action = PROVIDER_ACTION_BOOKMARK;
        else if (pFrag->State.eAction == SPVA_SpellOut) fragment.Action = PROVIDER_ACTION_SPELL_OUT;
        else if (pFrag->State.eAction == SPVA_Pronounce) fragment.Action = PROVIDER_ACTION_PRONOUNCE;
        else fragment.Action = pFrag->State.eAction;
        
        fragment.Volume = static_cast<float>(pFrag->State.Volume);
        fragment.Rate = static_cast<float>(pFrag->State.RateAdj);
        fragment.Pitch = static_cast<float>(pFrag->State.PitchAdj.MiddleAdj);
        
        fragments.push_back(fragment);
        fragmentOffsets.push_back(currentOffset);
        
        pFrag = pFrag->pNext;
    }
    
    // Pass 2: Assign unmanaged text pointers
    for (size_t i = 0; i < fragments.size(); i++)
    {
        fragments[i].Text = backingBuffer.data() + fragmentOffsets[i];
    }

    m_pWorker->Start(std::move(backingBuffer), std::move(fragments));

    while (!m_pWorker->IsFinished())
    {
        DWORD actions = 0;
        {
            std::lock_guard<std::mutex> lock(m_siteMutex);
            if (m_cpSite)
            {
                actions = m_cpSite->GetActions();
            }
        }
        if (actions & SPVES_ABORT)
        {
            m_pWorker->Stop();
            break;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    m_pWorker->WaitUntilFinished();
    {
        std::lock_guard<std::mutex> lock(m_siteMutex);
        m_cpSite = nullptr;
    }
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
    std::lock_guard<std::mutex> lock(m_siteMutex);
    if (!m_cpSite) return false;

    DWORD actions = m_cpSite->GetActions();
    if (actions & SPVES_ABORT) return false;
    
    ULONG cbWritten = 0;
    HRESULT hr = m_cpSite->Write((const void*)pAudioBytes, byteCount, &cbWritten);
    return SUCCEEDED(hr);
}

void CSapiEngine::OnSpeechEvent(const ProviderSpeechEvent* pEvent)
{
    if (!pEvent) return;

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

    std::lock_guard<std::mutex> lock(m_siteMutex);
    if (!m_cpSite) return;
    m_cpSite->AddEvents(&sapiEvent, 1);
}
