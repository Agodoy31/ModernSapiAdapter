/**
 * @file SapiEngine.h
 * @brief Core SAPI 5 COM proxy router implementation.
 */

#pragma once
#include "ProviderWrapper.h"
#include "SpeechWorker.h"

/**
 * @class CSapiEngine
 * @brief Implements SAPI 5 COM engine interfaces and proxies calls to unmanaged speech providers.
 */
class CSapiEngine : public winrt::implements<CSapiEngine, ISpTTSEngine, ISpObjectWithToken>
{
    friend class SapiEngineTests_OnSpeechEventMapsAndDispatchesToSite_Test;
public:
    CSapiEngine();
    ~CSapiEngine();

    IFACEMETHODIMP SetObjectToken(ISpObjectToken* pToken) noexcept override;
    IFACEMETHODIMP GetObjectToken(ISpObjectToken** ppToken) noexcept override;

    IFACEMETHODIMP Speak(DWORD dwSpeakFlags,
                         REFGUID rguidFormatId,
                         const WAVEFORMATEX* pWaveFormatEx,
                         const SPVTEXTFRAG* pTextFragList,
                         ISpTTSEngineSite* pOutputSite) noexcept override;

    IFACEMETHODIMP GetOutputFormat(const GUID* pTargetFmtId,
                                   const WAVEFORMATEX* pTargetWaveFormatEx,
                                   GUID* pOutputFormatId,
                                   WAVEFORMATEX** ppCoMemOutputWaveFormatEx) noexcept override;

    bool OnAudioData(const uint8_t* pAudioBytes, uint32_t byteCount);
    void OnSpeechEvent(const ProviderSpeechEvent* pEvent);

private:
    winrt::com_ptr<ISpObjectToken> m_cpToken;
    winrt::com_ptr<ISpTTSEngineSite> m_cpSite;

    std::unique_ptr<ProviderWrapper> m_pWrapper;
    std::unique_ptr<SpeechWorker> m_pWorker;

    HRESULT LoadProviderFromToken(ISpObjectToken* pToken);
};

