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
    static constexpr DWORD ControlOperationTimeoutMs = 1500;

    /**
     * @brief Constructs a new PipeClient instance.
     */
    PipeClient();

    /**
     * @brief Cleans up pipe handles. Provider lifetime is managed by the provider's idle policy.
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
     * @param json JSON payload to serialize and transmit.
     * @return S_OK on success, or HRESULT error code on failure.
     */
    HRESULT SendControlMessage(
        const nlohmann::json& json,
        DWORD timeoutMs = ControlOperationTimeoutMs);

    /**
     * @brief Reads and parses a newline-delimited JSON control message from the Control Pipe.
     * @param[out] outJson Parsed JSON output.
     * @return S_OK on success, or HRESULT error code on failure.
     */
    HRESULT ReadControlMessage(
        nlohmann::json& outJson,
        DWORD timeoutMs = INFINITE);

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

#if defined(_DEBUG)
    void FailNextCancellationMessageForTest();
    void FailNextSpeakMessageForTest();
#endif

private:
    wil::unique_handle m_controlPipe;               /**< Overlapped handle to Control Pipe. */
    wil::unique_handle m_audioPipe;                 /**< Overlapped handle to Audio Pipe. */
    wil::unique_process_information m_providerProcess; /**< Process information handle of launched provider. */
    std::string m_controlInputBuffer;                /**< Retained bytes after the most recently extracted JSON line. */
    size_t m_controlInputOffset{0};                  /**< Offset of unparsed data in control input buffer. */
    size_t m_controlSearchOffset{0};                 /**< Search offset to prevent O(N^2) scanning. */
    std::mutex m_controlWriteMutex;                  /**< Serializes newline-delimited JSON writes to the byte-mode control pipe. */
#if defined(_DEBUG)
    std::atomic_bool m_failNextCancellationMessageForTest{false}; /**< Test-only cancellation write failure injection. */
    std::atomic_bool m_failNextSpeakMessageForTest{false}; /**< Test-only initial speak write failure injection. */
#endif
    /**
     * @brief Internal helper to attempt connection to Control and Audio pipes.
     * @param controlPipePath Full pipe path for control channel.
     * @param audioPipePath Full pipe path for audio channel.
     * @param[out] controlPipeOpened True when the control pipe opened before a later failure.
     * @return S_OK on success, or HRESULT error code on failure.
     */
    HRESULT TryConnectPipes(
        const std::wstring& controlPipePath,
        const std::wstring& audioPipePath,
        bool& controlPipeOpened);

    HRESULT SendControlMessageUtf8(
        const std::string& utf8String,
        DWORD timeoutMs);

    HRESULT ReadControlMessageUtf8(
        std::string_view& utf8View,
        DWORD timeoutMs);

    void CompactControlBuffer();

    /**
     * @brief Completes or safely cancels one overlapped operation within its caller's deadline.
     */
    static HRESULT CompleteOverlappedOperation(
        HANDLE pipe,
        OVERLAPPED& overlapped,
        DWORD& bytesTransferred,
        DWORD timeoutMs) noexcept;
};
