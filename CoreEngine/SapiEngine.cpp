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
    if (!m_pWrapper || !m_pWrapper->IsLoaded())
    {
        CoreLog(L"[CoreEngine] GetOutputFormat failed: Wrapper not initialized.");
        return SPERR_UNINITIALIZED;
    }

    ProviderAudioFormat format{};
    if (!m_pWrapper->GetAudioFormat(&format))
    {
        CoreLog(L"[CoreEngine] GetOutputFormat failed: Provider GetAudioFormat failed.");
        return E_FAIL;
    }

    if (pOutputFormatId) *pOutputFormatId = SPDFID_WaveFormatEx;
    
    if (ppCoMemOutputWaveFormatEx)
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

    std::vector<char16_t> backingBuffer;
    std::vector<ProviderSpeechFragment> fragments;
    std::vector<uint32_t> fragmentOffsets;
    
    // Pass 1: Count total chars and fragment count to pre-allocate
    uint32_t totalChars = 0;
    uint32_t fragmentCount = 0;
    for (const SPVTEXTFRAG* p = pTextFragList; p; p = p->pNext)
    {
        totalChars += p->ulTextLen;
        fragmentCount++;
    }
    backingBuffer.reserve(totalChars + fragmentCount);

    const SPVTEXTFRAG* pFrag = pTextFragList;
    while (pFrag)
    {
        uint32_t currentOffset = static_cast<uint32_t>(backingBuffer.size());
        if (pFrag->pTextStart && pFrag->ulTextLen > 0)
        {
            backingBuffer.insert(backingBuffer.end(), (const char16_t*)pFrag->pTextStart, (const char16_t*)pFrag->pTextStart + pFrag->ulTextLen);
        }
        backingBuffer.push_back(u'\0');
        
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

    CoreLog(L"[CoreEngine] Starting worker thread. Fragments: %zu, VoiceId: %ls", fragments.size(), m_voiceId.c_str());
    m_pWorker->Start(std::move(backingBuffer), std::move(fragments), m_voiceId);

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
            CoreLog(L"[CoreEngine] Speak abort requested by SAPI site.");
            m_pWorker->Stop();
            
            // WAIT FOR WORKER BEFORE RETURNING TO SAPI!
            m_pWorker->WaitUntilFinished();
            
            {
                std::lock_guard<std::mutex> lock(m_siteMutex);
                m_cpSite = nullptr;
            }
            CoreLog(L"[CoreEngine] Speak aborted safely.");
            return S_OK;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    m_pWorker->WaitUntilFinished();
    {
        std::lock_guard<std::mutex> lock(m_siteMutex);
        m_cpSite = nullptr;
    }
    CoreLog(L"[CoreEngine] Speak completed.");
    return S_OK;
}
catch (const std::exception& e) { CoreLog(L"[CoreEngine] Speak exception: %hs", e.what()); return winrt::to_hresult(); }
catch (...) { CoreLog(L"[CoreEngine] Speak unknown exception."); return winrt::to_hresult(); }

HRESULT CSapiEngine::LoadProviderFromToken(ISpObjectToken* pToken)
{
    wil::com_ptr<ISpDataKey> cpDataKey;
    HRESULT hr = pToken->QueryInterface(IID_PPV_ARGS(&cpDataKey));
    if (FAILED(hr))
    {
        CoreLog(L"[CoreEngine] LoadProviderFromToken: QueryInterface(ISpDataKey) failed (0x%08X)", hr);
        return hr;
    }

    wil::unique_cotaskmem_string pVoiceId;
    if (SUCCEEDED(cpDataKey->GetStringValue(L"VoiceId", &pVoiceId)) && pVoiceId)
    {
        m_voiceId = pVoiceId.get();
    }
    else
    {
        CoreLog(L"[CoreEngine] LoadProviderFromToken: VoiceId not found in token");
    }

    wil::unique_cotaskmem_string pDllPath;
    hr = cpDataKey->GetStringValue(L"ProviderDLL", &pDllPath);
    if (FAILED(hr))
    {
        CoreLog(L"[CoreEngine] LoadProviderFromToken: ProviderDLL not found in token (0x%08X)", hr);
        return E_INVALIDARG;
    }

    CoreLog(L"[CoreEngine] Loading ProviderDLL: %ls (VoiceId: %ls)", pDllPath.get(), m_voiceId.c_str());
    HRESULT loadHr = m_pWrapper->Load(pDllPath.get());
    if (FAILED(loadHr))
    {
        CoreLog(L"[CoreEngine] ProviderWrapper::Load failed with hr=0x%08X for path %ls", loadHr, pDllPath.get());
    }
    else
    {
        CoreLog(L"[CoreEngine] ProviderWrapper::Load succeeded for %ls", pDllPath.get());
    }
    return loadHr;
}

bool CSapiEngine::OnAudioData(const uint8_t* pAudioBytes, uint32_t byteCount)
{
    winrt::com_ptr<ISpTTSEngineSite> cpSite;
    {
        std::lock_guard<std::mutex> lock(m_siteMutex);
        if (!m_cpSite) return false;
        cpSite = m_cpSite;
    }

    DWORD actions = cpSite->GetActions();
    if (actions & SPVES_ABORT) return false;
    
    ULONG cbWritten = 0;
    HRESULT hr = cpSite->Write((const void*)pAudioBytes, byteCount, &cbWritten);
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
        sapiEvent.lParam = pEvent->TextOffset;
        sapiEvent.wParam = pEvent->TextLength;
        break;
    case PROVIDER_EVENT_SENTENCE_BOUNDARY:
        sapiEvent.eEventId = SPEI_SENTENCE_BOUNDARY;
        sapiEvent.lParam = pEvent->TextOffset;
        sapiEvent.wParam = pEvent->TextLength;
        break;
    case PROVIDER_EVENT_BOOKMARK:
        sapiEvent.eEventId = SPEI_TTS_BOOKMARK;
        if (pEvent->StringData != nullptr && pEvent->StringData[0] != u'\0') {
            sapiEvent.elParamType = SPET_LPARAM_IS_STRING;
            const wchar_t* pStr = reinterpret_cast<const wchar_t*>(pEvent->StringData);
            size_t len = wcslen(pStr);
            // SAPI takes ownership of this allocation when elParamType is SPET_LPARAM_IS_STRING.
            wchar_t* pCopy = static_cast<wchar_t*>(CoTaskMemAlloc((len + 1) * sizeof(wchar_t)));
            if (pCopy)
            {
                wcscpy_s(pCopy, len + 1, pStr);
            }
            sapiEvent.lParam = reinterpret_cast<LPARAM>(pCopy);
            sapiEvent.wParam = static_cast<WPARAM>(_wtol(pStr));
        } else {
            sapiEvent.elParamType = SPET_LPARAM_IS_UNDEFINED;
            sapiEvent.lParam = pEvent->TextLength;
            sapiEvent.wParam = pEvent->TextOffset;
        }
        break;
    default:
        // Unsupported event
        return;
    }

    winrt::com_ptr<ISpTTSEngineSite> cpSite;
    {
        std::lock_guard<std::mutex> lock(m_siteMutex);
        if (!m_cpSite)
        {
            if (sapiEvent.elParamType == SPET_LPARAM_IS_STRING && sapiEvent.lParam)
            {
                CoTaskMemFree(reinterpret_cast<void*>(sapiEvent.lParam));
            }
            return;
        }
        cpSite = m_cpSite;
    }
    cpSite->AddEvents(&sapiEvent, 1);
}
