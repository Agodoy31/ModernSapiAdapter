#include "pch.h"
#include "SpeechWorker.h"
#include "SapiEngine.h"
#include "ProviderWrapper.h"

SpeechWorker::SpeechWorker(CSapiEngine* pEngine, ProviderWrapper* pWrapper)
    : m_pEngine(pEngine), m_pWrapper(pWrapper)
{
}

SpeechWorker::~SpeechWorker()
{
    WaitUntilFinished();
}

void SpeechWorker::Start(const std::u16string& text)
{
    WaitUntilFinished(); // Ensure previous is joined
    m_abortFlag = 0;
    m_isRunning = true;
    m_thread = std::thread(&SpeechWorker::ThreadProc, this, text);
}

void SpeechWorker::Stop()
{
    m_abortFlag = 1;
}

void SpeechWorker::WaitUntilFinished()
{
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

void SpeechWorker::ThreadProc(std::u16string text)
{
    ProviderSpeakParams params{};
    params.ContractVersion = PROVIDER_ABI_VERSION;
    params.TextBuffer = text.c_str();
    params.TextLength = static_cast<uint32_t>(text.length());
    params.ActionType = PROVIDER_ACTION_SPEAK;
    params.SpeechRate = 0.0f;
    params.Volume = 100.0f;
    params.Pitch = 0.0f;
    params.pAbortFlag = &m_abortFlag;
    params.UserContext = this;
    params.AudioCallback = &SpeechWorker::AudioCallback;
    params.MetaCallback = &SpeechWorker::MetaCallback;

    m_pWrapper->Speak(&params);
    m_isRunning = false;
}

bool __stdcall SpeechWorker::AudioCallback(const uint8_t* pAudioBytes, uint32_t byteCount, void* ctx)
{
    auto* worker = static_cast<SpeechWorker*>(ctx);
    if (!worker || worker->m_abortFlag) return false;
    return worker->m_pEngine->OnAudioData(pAudioBytes, byteCount);
}

void __stdcall SpeechWorker::MetaCallback(const ProviderSpeechEvent* pEvent, void* ctx)
{
    auto* worker = static_cast<SpeechWorker*>(ctx);
    if (!worker || worker->m_abortFlag) return;
    worker->m_pEngine->OnSpeechEvent(pEvent);
}
