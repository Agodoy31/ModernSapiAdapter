#include "pch.h"
#include "SpeechWorker.h"
#include "SapiEngine.h"

SpeechWorker::SpeechWorker(CSapiEngine* pEngine, std::shared_ptr<PipeClient> pClient)
    : m_pEngine(pEngine), m_pClient(pClient), m_exit(false), m_isSpeaking(false)
{
    m_audioThread = std::thread(&SpeechWorker::AudioThreadProc, this);
    m_controlThread = std::thread(&SpeechWorker::ControlThreadProc, this);
}

SpeechWorker::~SpeechWorker()
{
    m_exit = true;
    
    // Cancel I/O so blocking ReadFile/GetOverlappedResult returns immediately
    if (m_pClient)
    {
        m_pClient->Cancel();
    }

    if (m_audioThread.joinable()) m_audioThread.join();
    if (m_controlThread.joinable()) m_controlThread.join();
}

void SpeechWorker::Start(void* /*pSite*/)
{
    m_isSpeaking = true;
}

void SpeechWorker::Stop()
{
    if (m_isSpeaking)
    {
        using namespace winrt::Windows::Data::Json;
        JsonObject req;
        req.SetNamedValue(L"command", JsonValue::CreateStringValue(L"cancel"));
        req.SetNamedValue(L"speak_id", JsonValue::CreateNumberValue(1));
        m_pClient->SendControlMessage(req);
        m_isSpeaking = false;
    }
}

void SpeechWorker::WaitUntilFinished()
{
    while (m_isSpeaking && !m_exit)
    {
        Sleep(10);
    }
}

void SpeechWorker::AudioThreadProc()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<uint8_t> buffer(4096);
    while (!m_exit)
    {
        DWORD bytesRead = 0;
        HRESULT hr = m_pClient->ReadAudioChunk(buffer, bytesRead);
        if (FAILED(hr))
        {
            Sleep(50);
            continue;
        }
        
        if (m_isSpeaking && bytesRead > 0)
        {
            m_pEngine->OnAudioData(buffer.data(), bytesRead);
        }
    }
    CoUninitialize();
}

void SpeechWorker::ControlThreadProc()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (!m_exit)
    {
        winrt::Windows::Data::Json::JsonObject json = nullptr;
        HRESULT hr = m_pClient->ReadControlMessage(json);
        if (FAILED(hr) || !json)
        {
            Sleep(50);
            continue;
        }

        if (json.HasKey(L"event"))
        {
            auto eventStr = json.GetNamedString(L"event");
            if (eventStr == L"completed" || eventStr == L"error")
            {
                m_isSpeaking = false;
            }
        }

        m_pEngine->OnSpeechEvent(json);
    }
    CoUninitialize();
}
