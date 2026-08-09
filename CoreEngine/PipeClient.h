/**
 * @file PipeClient.h
 * @brief Manages out-of-process Named Pipe IPC transport and provider process lifecycle.
 */

#pragma once
#include "pch.h"

/**
 * @class PipeClient
 * @brief Handles dual Named Pipe connections (Control and Audio) and provider process management.
 */
class PipeClient
{
public:
    /**
     * @brief Constructs a new PipeClient instance.
     */
    PipeClient();

    /**
     * @brief Cleans up pipe handles and terminates the underlying provider process.
     */
    ~PipeClient();

    /**
     * @brief Connects to the Control and Audio pipes of a provider, launching the process if needed.
     * @param pipeName Base name of the target named pipe.
     * @param exePath Absolute filepath to the provider executable.
     * @return S_OK on success, or Win32/HRESULT error code on failure.
     */
    HRESULT Connect(const std::wstring& pipeName, const std::wstring& exePath);

    /**
     * @brief Sends a JSON control message over the Control Pipe.
     * @param json JsonObject payload to serialize and transmit.
     * @return S_OK on success, or HRESULT error code on failure.
     */
    HRESULT SendControlMessage(const winrt::Windows::Data::Json::JsonObject& json);

    /**
     * @brief Reads and parses a newline-delimited JSON control message from the Control Pipe.
     * @param[out] outJson Parsed JsonObject output.
     * @return S_OK on success, or HRESULT error code on failure.
     */
    HRESULT ReadControlMessage(winrt::Windows::Data::Json::JsonObject& outJson);

    /**
     * @brief Reads a raw PCM audio chunk from the Audio Pipe.
     * @param[out] buffer Output byte vector populated with audio bytes.
     * @param[out] bytesRead Output count of bytes read.
     * @return S_OK on success, or HRESULT error code on failure.
     */
    HRESULT ReadAudioChunk(std::vector<uint8_t>& buffer, DWORD& bytesRead);

    /**
     * @brief Cancels pending overlapped I/O operations on Control and Audio pipes.
     */
    void Cancel();

private:
    wil::unique_handle m_controlPipe;               /**< Overlapped handle to Control Pipe. */
    wil::unique_handle m_audioPipe;                 /**< Overlapped handle to Audio Pipe. */
    wil::unique_process_information m_providerProcess; /**< Process information handle of launched provider. */
    std::string m_controlInputBuffer;                /**< Retained bytes after the most recently extracted JSON line. */
    std::mutex m_controlWriteMutex;                  /**< Serializes newline-delimited JSON writes to the byte-mode control pipe. */

    /**
     * @brief Internal helper to attempt connection to Control and Audio pipes.
     * @param controlPipePath Full pipe path for control channel.
     * @param audioPipePath Full pipe path for audio channel.
     * @return S_OK on success, or HRESULT error code on failure.
     */
    HRESULT TryConnectPipes(const std::wstring& controlPipePath, const std::wstring& audioPipePath);
};
