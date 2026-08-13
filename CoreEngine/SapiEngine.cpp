#include "pch.h"
#include "SapiEngine.h"

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
    std::lock_guard<std::mutex> speakLock(m_speakMutex);
    {
        std::lock_guard<std::mutex> lock(m_siteMutex);
        m_cpSite = nullptr;
    }

    std::lock_guard<std::mutex> sessionLock(m_sessionMutex);
    m_pWorker.reset();

    m_pClient.reset();
}

IFACEMETHODIMP CSapiEngine::SetObjectToken(ISpObjectToken* pToken) noexcept try
{
    CoreLog(L"[CoreEngine] SetObjectToken called.");
    if (!pToken) return E_POINTER;
    std::lock_guard<std::mutex> speakLock(m_speakMutex);
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
    std::lock_guard<std::mutex> sessionLock(m_sessionMutex);
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

    std::lock_guard<std::mutex> speakLock(m_speakMutex);

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
            RetireFaultedSessionLocked();
            if (FAILED(CreateProviderSessionLocked()))
            {
                return E_FAIL;
            }
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

    HRESULT hr = client->SendControlMessage(speakReq);
    if (FAILED(hr))
    {
        worker->EnterFaultedState();
        return hr;
    }

    // A transport failure leaves the session Faulted with outstanding pipe I/O cancelled.
    // The next Speak owns worker/client retirement and replacement under the session lifecycle lock.
    return worker->WaitUntilFinished(pOutputSite);
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
    std::lock_guard<std::mutex> lock(m_siteMutex);
    if (!m_cpSite) return;

    if (!eventJson.contains("event") || !eventJson["event"].is_string()) return;
    auto eventStr = eventJson["event"].get<std::string>();
    
    if (eventJson.contains("speak_id") && eventJson["speak_id"].is_number_integer())
    {
        uint64_t eventSpeakId = eventJson["speak_id"].get<uint64_t>();
        if (eventSpeakId != m_speakIdCounter.load())
        {
            return; // Drop stale event
        }
    }

    if (eventStr == "word_boundary")
    {
        SPEVENT spEvent = {};
        spEvent.eEventId = SPEI_WORD_BOUNDARY;
        spEvent.elParamType = SPET_LPARAM_IS_UNDEFINED;
        
        uint32_t audioMs = (eventJson.contains("audio_offset_ms") && eventJson["audio_offset_ms"].is_number_unsigned()) ? eventJson["audio_offset_ms"].get<uint32_t>() : 0;
        spEvent.ullAudioStreamOffset = AudioOffsetMsToBytes(audioMs);

        uint32_t textOffset = (eventJson.contains("text_offset") && eventJson["text_offset"].is_number_unsigned()) ? eventJson["text_offset"].get<uint32_t>() : 0;
        uint32_t textLength = (eventJson.contains("text_length") && eventJson["text_length"].is_number_unsigned()) ? eventJson["text_length"].get<uint32_t>() : 0;
        spEvent.wParam = static_cast<WPARAM>(textLength);
        spEvent.lParam = static_cast<LPARAM>(textOffset);
        
        m_cpSite->AddEvents(&spEvent, 1);
    }
    else if (eventStr == "sentence_boundary")
    {
        SPEVENT spEvent = {};
        spEvent.eEventId = SPEI_SENTENCE_BOUNDARY;
        spEvent.elParamType = SPET_LPARAM_IS_UNDEFINED;
        
        uint32_t audioMs = (eventJson.contains("audio_offset_ms") && eventJson["audio_offset_ms"].is_number_unsigned()) ? eventJson["audio_offset_ms"].get<uint32_t>() : 0;
        spEvent.ullAudioStreamOffset = AudioOffsetMsToBytes(audioMs);

        uint32_t textOffset = (eventJson.contains("text_offset") && eventJson["text_offset"].is_number_unsigned()) ? eventJson["text_offset"].get<uint32_t>() : 0;
        uint32_t textLength = (eventJson.contains("text_length") && eventJson["text_length"].is_number_unsigned()) ? eventJson["text_length"].get<uint32_t>() : 0;
        spEvent.wParam = static_cast<WPARAM>(textLength);
        spEvent.lParam = static_cast<LPARAM>(textOffset);
        
        m_cpSite->AddEvents(&spEvent, 1);
    }
    else if (eventStr == "bookmark_reached")
    {
        SPEVENT spEvent = {};
        spEvent.eEventId = SPEI_TTS_BOOKMARK;
        
        uint32_t audioMs = (eventJson.contains("audio_offset_ms") && eventJson["audio_offset_ms"].is_number_unsigned()) ? eventJson["audio_offset_ms"].get<uint32_t>() : 0;
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
        
        m_cpSite->AddEvents(&spEvent, 1);
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

        const auto& format = infoResponse["audio_format"];
        const auto isPositiveInteger = [&format](const char* name, uint64_t maximum, uint64_t& value) {
            if (!format.contains(name) || !format[name].is_number_unsigned())
            {
                return false;
            }

            const uint64_t number = format[name].get<uint64_t>();
            if (number == 0 || number > maximum)
            {
                return false;
            }

            value = number;
            return true;
        };

        uint64_t sampleRate = 0;
        uint64_t bitsPerSample = 0;
        uint64_t channels = 0;
        if (!isPositiveInteger("sample_rate", (std::numeric_limits<DWORD>::max)(), sampleRate) ||
            !isPositiveInteger("bits_per_sample", (std::numeric_limits<WORD>::max)(), bitsPerSample) ||
            !isPositiveInteger("channels", (std::numeric_limits<WORD>::max)(), channels))
        {
            return E_FAIL;
        }

        const uint64_t blockAlignment = (channels * bitsPerSample) / 8;
        if (blockAlignment == 0 || blockAlignment > (std::numeric_limits<WORD>::max)() ||
            channels * bitsPerSample != blockAlignment * 8 ||
            sampleRate > (std::numeric_limits<DWORD>::max)() / blockAlignment)
        {
            return E_FAIL;
        }

        WAVEFORMATEX candidateFormat = {};
        candidateFormat.wFormatTag = WAVE_FORMAT_PCM;
        candidateFormat.nSamplesPerSec = static_cast<DWORD>(sampleRate);
        candidateFormat.wBitsPerSample = static_cast<WORD>(bitsPerSample);
        candidateFormat.nChannels = static_cast<WORD>(channels);
        candidateFormat.nBlockAlign = static_cast<WORD>(blockAlignment);
        candidateFormat.nAvgBytesPerSec = candidateFormat.nSamplesPerSec * candidateFormat.nBlockAlign;
        candidateFormat.cbSize = 0;

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
