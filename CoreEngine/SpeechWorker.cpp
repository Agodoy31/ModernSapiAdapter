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

void SpeechWorker::Start(void* /*pSite*/, uint64_t speakId)
{
    m_activeSpeakId = speakId;
    m_isSpeaking = true;
}

void SpeechWorker::Stop()
{
    if (m_isSpeaking)
    {
        using namespace winrt::Windows::Data::Json;
        JsonObject req;
        req.SetNamedValue(L"command", JsonValue::CreateStringValue(L"cancel"));
        req.SetNamedValue(L"speak_id", JsonValue::CreateNumberValue(static_cast<double>(m_activeSpeakId.load())));
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
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    std::vector<uint8_t> buffer(4096);
    while (!m_exit)
    {
        DWORD bytesRead = 0;
        HRESULT hr = m_pClient->ReadAudioChunk(buffer, bytesRead);
        if (FAILED(hr))
        {
            if (m_exit) break;
            Sleep(50);
            continue;
        }
        
        if (m_isSpeaking && bytesRead > 0)
        {
            m_pEngine->OnAudioData(buffer.data(), bytesRead);
        }
    }
    winrt::uninit_apartment();
}

void SpeechWorker::ControlThreadProc()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    while (!m_exit)
    {
        winrt::Windows::Data::Json::JsonObject json = nullptr;
        HRESULT hr = m_pClient->ReadControlMessage(json);
        if (FAILED(hr) || !json)
        {
            if (m_exit) break;
            Sleep(50);
            continue;
        }

        try
        {
            if (json.HasKey(L"event") && json.HasKey(L"speak_id"))
            {
                if (json.GetNamedValue(L"event").ValueType() == winrt::Windows::Data::Json::JsonValueType::String &&
                    json.GetNamedValue(L"speak_id").ValueType() == winrt::Windows::Data::Json::JsonValueType::Number)
                {
                    auto eventStr = json.GetNamedString(L"event");
                    uint64_t eventSpeakId = static_cast<uint64_t>(json.GetNamedNumber(L"speak_id"));
                    
                    if (eventSpeakId == m_activeSpeakId.load())
                    {
                        if (eventStr == L"completed")
                        {
                            m_isSpeaking = false;
                        }
                        else if (eventStr == L"error")
                        {
                            if (json.HasKey(L"severity") && json.GetNamedValue(L"severity").ValueType() == winrt::Windows::Data::Json::JsonValueType::String)
                            {
                                auto severity = json.GetNamedString(L"severity");
                                if (severity == L"error" || severity == L"fatal")
                                {
                                    m_isSpeaking = false;
                                }
                            }
                            else
                            {
                                m_isSpeaking = false; // Default to fatal if no severity specified or invalid type
                            }
                        }
                    }
                }
            }

            m_pEngine->OnSpeechEvent(json);
        }
        catch (const std::exception& e)
        {
            CoreLog(L"[SpeechWorker] Exception in ControlThreadProc: %hs", e.what());
        }
        catch (...)
        {
            CoreLog(L"[SpeechWorker] Unknown exception in ControlThreadProc.");
        }
    }
    winrt::uninit_apartment();
}
