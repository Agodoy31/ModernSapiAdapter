#pragma once
#include "pch.h"

class PipeClient
{
public:
    PipeClient() = default;
    ~PipeClient() = default;

    HRESULT Connect(const std::wstring& pipeName, const std::wstring& exePath);
    HRESULT SendControlMessage(const winrt::Windows::Data::Json::JsonObject& json);
    HRESULT ReadControlMessage(winrt::Windows::Data::Json::JsonObject& outJson);
    HRESULT ReadAudioChunk(std::vector<uint8_t>& buffer, DWORD& bytesRead);
    void Cancel();

private:
    wil::unique_handle m_controlPipe;
    wil::unique_handle m_audioPipe;
    wil::unique_process_information m_providerProcess;

    HRESULT TryConnectPipes(const std::wstring& controlPipePath, const std::wstring& audioPipePath);
};
