/**
 * @file ProviderWrapper.h
 * @brief Handles dynamic loading and ABI function pointer dispatch for provider DLLs.
 */

#pragma once

/**
 * @class ProviderWrapper
 * @brief Manages the lifecycle of an unmanaged provider DLL module and provides type-safe ABI execution calls.
 */
class ProviderWrapper
{
public:
    ProviderWrapper() = default;
    ~ProviderWrapper() = default;

    ProviderWrapper(const ProviderWrapper&) = delete;
    ProviderWrapper& operator=(const ProviderWrapper&) = delete;

    HRESULT Load(const std::wstring& dllPath);
    void Unload();
    bool IsLoaded() const;
    bool GetAudioFormat(ProviderAudioFormat* format);
    bool Speak(const ProviderSpeakParams* params);

private:
    wil::unique_hmodule m_module;
    PFN_GET_PROVIDER_ABI_VERSION m_pfnGetVersion = nullptr;
    PFN_GET_PROVIDER_AUDIO_FORMAT m_pfnGetAudioFormat = nullptr;
    PFN_PROVIDER_SPEAK m_pfnSpeak = nullptr;
};

