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

void SpeechWorker::Start(std::vector<char16_t> backingBuffer, std::vector<ProviderSpeechFragment> fragments, std::wstring voiceId)
{
    WaitUntilFinished(); // Ensure previous is joined
    m_abortFlag = 0;
    m_isRunning = true;
    m_thread = std::thread(&SpeechWorker::ThreadProc, this, std::move(backingBuffer), std::move(fragments), std::move(voiceId));
}

void SpeechWorker::Stop()
{
    m_abortFlag = 1;
}

void SpeechWorker::WaitUntilFinished()
{
    if (m_thread.joinable())
    {
        HANDLE hThread = (HANDLE)m_thread.native_handle();
        DWORD dwIndex = 0;
        // MUST pump COM messages while waiting to avoid deadlocking WASAPI!
        // If we just call m_thread.join(), the STA thread freezes, which can lock up audiosrv system-wide.
        CoWaitForMultipleHandles(0, INFINITE, 1, &hThread, &dwIndex);
        m_thread.join(); // Safe to join now since the thread has exited
    }
}

void SpeechWorker::ThreadProc(std::vector<char16_t> backingBuffer, std::vector<ProviderSpeechFragment> fragments, std::wstring voiceId)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    
    ProviderSpeakParams params{};
    params.ContractVersion = PROVIDER_ABI_VERSION;
    params.Fragments = fragments.data();
    params.FragmentCount = static_cast<uint32_t>(fragments.size());
    params.VoiceModel = reinterpret_cast<const char16_t*>(voiceId.c_str());
    params.pAbortFlag = &m_abortFlag;
    params.UserContext = this;
    params.AudioCallback = &SpeechWorker::AudioCallback;
    params.MetaCallback = &SpeechWorker::MetaCallback;

    m_pWrapper->Speak(&params);
    
    m_isRunning = false;
    CoUninitialize();
}

bool __stdcall SpeechWorker::AudioCallback(const uint8_t* pAudioBytes, uint32_t byteCount, void* ctx)
{
    try {
        auto* worker = static_cast<SpeechWorker*>(ctx);
        if (!worker || worker->m_abortFlag) return false;
        return worker->m_pEngine->OnAudioData(pAudioBytes, byteCount);
    } catch (...) {
        return false;
    }
}

void __stdcall SpeechWorker::MetaCallback(const ProviderSpeechEvent* pEvent, void* ctx)
{
    try {
        auto* worker = static_cast<SpeechWorker*>(ctx);
        if (!worker || worker->m_abortFlag) return;
        worker->m_pEngine->OnSpeechEvent(pEvent);
    } catch (...) {
        // Suppress exceptions crossing ABI boundary
    }
}
