#include "pch.h"
#include "ProviderWrapper.h"

HRESULT ProviderWrapper::Load(const std::wstring& dllPath)
{
    m_module.reset(LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
    if (!m_module)
    {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        CoreLog(L"[ProviderWrapper] LoadLibraryExW failed with hr=0x%08X for path %ls", hr, dllPath.c_str());
        return hr;
    }

    m_pfnGetVersion = (PFN_GET_PROVIDER_ABI_VERSION)GetProcAddress(m_module.get(), "GetProviderAbiVersion");
    m_pfnGetAudioFormat = (PFN_GET_PROVIDER_AUDIO_FORMAT)GetProcAddress(m_module.get(), "GetProviderAudioFormat");
    m_pfnSpeak = (PFN_PROVIDER_SPEAK)GetProcAddress(m_module.get(), "ProviderSpeak");

    if (!m_pfnGetVersion || !m_pfnGetAudioFormat || !m_pfnSpeak)
    {
        m_module.reset();
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    }

    uint32_t version = m_pfnGetVersion();
    if (version != PROVIDER_ABI_VERSION)
    {
        m_module.reset();
        return E_INVALIDARG;
    }

    return S_OK;
}

void ProviderWrapper::Unload()
{
    m_pfnGetVersion = nullptr;
    m_pfnGetAudioFormat = nullptr;
    m_pfnSpeak = nullptr;
    m_module.reset();
}

bool ProviderWrapper::IsLoaded() const
{
    return m_module.is_valid();
}

bool ProviderWrapper::GetAudioFormat(ProviderAudioFormat* format)
{
    if (!m_pfnGetAudioFormat || !format) return false;
    return m_pfnGetAudioFormat(format);
}

bool ProviderWrapper::Speak(const ProviderSpeakParams* params)
{
    if (!m_pfnSpeak) return false;
    return m_pfnSpeak(params);
}
