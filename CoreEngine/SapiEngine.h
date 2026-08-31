/**
 * @file SapiEngine.h
 * @brief Core SAPI 5 COM proxy router implementation.
 */

#pragma once
#include "PipeClient.h"
#include "SpeechWorker.h"

/**
 * @class CSapiEngine
 * @brief Implements SAPI 5 COM engine interfaces and proxies calls to unmanaged speech providers.
 */
class CSapiEngine : public winrt::implements<CSapiEngine, ISpTTSEngine, ISpObjectWithToken>
{
    friend class SapiEngineTests_OnSpeechEventMapsAndDispatchesToSite_Test;
    friend class SapiEngineTests_OnSpeechEventMapsSentenceBoundaryToSite_Test;
    friend class SapiEngineTests_OnSpeechEventMapsBookmarkStringEventToSite_Test;
    friend class SapiEngineTests_OnSpeechEventPreservesLongAudioOffsets_Test;
    friend class SapiEngineTests_RejectedAudioWriteWithFailedCancellationQuarantinesWorker_Test;
    friend class SapiEngineTests_CreateProviderSessionDoesNotPublishAnInvalidInfoResponse_Test;
    friend class SapiEngineTests_CreateProviderSessionAcceptsIntegralFloatAudioFormatNumbers_Test;
    friend class SapiEngineTests_FaultedSessionDoesNotForwardAnEventPausedBeforeItsSapiCallback_Test;
    friend class SapiEngineTests_OnSpeechEventAlignsOffsetsToPcmFrames_Test;
    friend class SapiEngineTests_CancellationDiscardsCarriedPcmBeforeTheNextRequest_Test;
    friend class SapiEngineTests_TerminalBeforeOverrunAudioForwardsOnlyDeclaredFrames_Test;
    friend class SapiEngineTests_WorkerReassemblesAwkward24BitStereoPipeFragments_Test;
    friend class SapiEngineTests_FrameAssemblyFailureFaultsWorkerWithoutEscapingThread_Test;
    friend class SapiEngineTests_AbortObservationRejectsPcmBeforeCancellationTransportStarts_Test;
    friend class SapiEngineTests_CancellationRejectsAnInitiallyApprovedEventAtTheSapiBoundary_Test;
    friend class SapiEngineTests_SynthesisCompleteWaitsForFinalSapiWriteToFinish_Test;
    friend class SapiEngineTests_SpeakCancelsPromptlyEvenWhenOutputSiteWriteBlocks_Test;
    friend class SapiEngineTests_SynthesisCompleteWhileCancellingCompletesPromptly_Test;
    friend class SapiEngineTests_WarningLogDoesNotFaultSession_Test;
    friend class SapiEngineTests_RequestErrorFailsUtteranceWithoutKillingProvider_Test;
    friend class SapiEngineTests_FatalErrorFaultsSessionAndTriggersRestart_Test;
    friend class SapiEngineTests_GetObjectTokenDoesNotWaitForActiveSpeakSerialization_Test;
public:
    /**
     * @brief Constructs a new CSapiEngine instance.
     */
    CSapiEngine();

    /**
     * @brief Destructs CSapiEngine and shuts down active worker threads.
     */
    ~CSapiEngine();

    /**
     * @brief Associates a SAPI object token with the engine, triggering provider initialization.
     * @param pToken Pointer to ISpObjectToken containing provider registry parameters.
     * @return S_OK on success, or HRESULT error code on failure.
     */
    IFACEMETHODIMP SetObjectToken(ISpObjectToken* pToken) noexcept override;

    /**
     * @brief Retrieves the current SAPI object token associated with the engine.
     * @param[out] ppToken Pointer receiving the ISpObjectToken interface.
     * @return S_OK on success, or HRESULT error code on failure.
     */
    IFACEMETHODIMP GetObjectToken(ISpObjectToken** ppToken) noexcept override;

    /**
     * @brief Synthesizes speech text fragments via provider IPC and streams output to SAPI site.
     * @param dwSpeakFlags Flags controlling speech synthesis execution (SPF_ASYNC, etc.).
     * @param rguidFormatId Format GUID requested by caller.
     * @param pWaveFormatEx Pointer to WAVEFORMATEX structure.
     * @param pTextFragList Pointer to linked list of SAPI text fragments.
     * @param pOutputSite Pointer to ISpTTSEngineSite receiving audio and events.
     * @return S_OK on success, or SPERR/HRESULT error code on failure.
     */
    IFACEMETHODIMP Speak(DWORD dwSpeakFlags,
                         REFGUID rguidFormatId,
                         const WAVEFORMATEX* pWaveFormatEx,
                         const SPVTEXTFRAG* pTextFragList,
                         ISpTTSEngineSite* pOutputSite) noexcept override;

    /**
     * @brief Queries supported output audio format from provider.
     * @param pTargetFmtId Target format GUID requested.
     * @param pTargetWaveFormatEx Target wave format requested.
     * @param[out] pOutputFormatId Output format GUID supported by provider.
     * @param[out] ppCoMemOutputWaveFormatEx CoTaskMemAlloc-allocated WAVEFORMATEX output.
     * @return S_OK on success, or SPERR/HRESULT error code on failure.
     */
    IFACEMETHODIMP GetOutputFormat(const GUID* pTargetFmtId,
                                   const WAVEFORMATEX* pTargetWaveFormatEx,
                                   GUID* pOutputFormatId,
                                   WAVEFORMATEX** ppCoMemOutputWaveFormatEx) noexcept override;

    /**
     * @brief Forwards raw PCM audio bytes to the active ISpTTSEngineSite.
     * @param pAudioBytes Pointer to raw PCM audio buffer.
     * @param byteCount Number of bytes in audio buffer.
     * @return true on successful write, false on failure.
     */
    bool OnAudioData(const uint8_t* pAudioBytes, uint32_t byteCount);

    /**
     * @brief Parses incoming JSON event payloads and translates them into SAPI SPEVENT notifications.
     * @param eventJson JSON event payload received from Control Pipe.
     */
    void OnSpeechEvent(const nlohmann::json& eventJson);

#if defined(_DEBUG)
    /**
     * @brief Injects one cancellation-control send failure for the focused worker recovery regression.
     */
    void FailNextCancellationControlSendForTest();
    void FailNextSpeakControlSendForTest();
#endif

private:
    mutable std::mutex m_tokenMutex;
    winrt::com_ptr<ISpObjectToken> m_cpToken;
    winrt::com_ptr<ISpTTSEngineSite> m_cpSite;
    std::mutex m_siteMutex;

    std::mutex m_sessionMutex;
    std::mutex m_speakMutex;
    std::unique_ptr<PipeClient> m_pClient;
    std::unique_ptr<SpeechWorker> m_pWorker;
    std::wstring m_providerExecutablePath;
    std::wstring m_providerPipeName;
    std::wstring m_voiceId;
    WAVEFORMATEX m_audioFormat;
    bool m_hasOutputFormat{false};
    std::atomic<uint64_t> m_speakIdCounter{0};

    uint64_t AudioOffsetMsToBytes(uint32_t audioMs) const;
    HRESULT LoadProviderFromToken(ISpObjectToken* pToken);
    HRESULT CreateProviderSessionLocked();
    void RetireFaultedSessionLocked();

    [[nodiscard]] static SapiSpeechEventType ParseSpeechEventType(std::string_view name) noexcept;
    void DispatchBoundaryEvent(const nlohmann::json& json, SPEVENTENUM eventId);
    void DispatchBookmarkEvent(const nlohmann::json& json);
    void DispatchLogEvent(const nlohmann::json& json);

    [[nodiscard]] static bool IsValidInfoResponse(const nlohmann::json& response) noexcept;
    [[nodiscard]] bool IsFormatCompatible(const WAVEFORMATEX& candidate) const noexcept;
};

