#include "pch.h"
#include <provider_abi.h>
#include "SynthesizerPool.h"
#include "AzureEmbeddedSynthesizer.h"
#include "Logger.h"
#include "VoiceManager.h"
#include "PcmCache.h"

#define PROVIDER_EXPORTS

void GlobalLazyInit() {
    static std::once_flag s_initFlag;
    std::call_once(s_initFlag, []() {
        LogInit();
        PcmCache::LoadFromDisk();
    });
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        // DO NOT call DisableThreadLibraryCalls(hModule) here!
        // The C++ Runtime (CRT) relies on DLL_THREAD_ATTACH to initialize thread-local storage (TLS).
        
        // Pin the DLL in memory to prevent the OS from unloading it and deadlocking on the loader lock
        HMODULE hPinnedModule = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&DllMain), &hPinnedModule);
        break;
    }
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    case DLL_PROCESS_DETACH:
        if (lpReserved == nullptr) {
            SynthesizerPool::Shutdown();
            PcmCache::SaveToDisk();
            LogShutdown();
        }
        break;
    }
    return TRUE;
}

extern "C" {

    __declspec(dllexport) uint32_t __stdcall GetProviderAbiVersion(void)
    {
        return PROVIDER_ABI_VERSION;
    }

    __declspec(dllexport) bool __stdcall GetProviderAudioFormat(ProviderAudioFormat* format)
    {
        if (!format) return false;
        format->SampleRate = 24000;
        format->BitsPerSample = 16;
        format->Channels = 1;
        format->Reserved = 0;
        return true;
    }

    __declspec(dllexport) bool __stdcall ProviderSpeak(const ProviderSpeakParams* params)
    {
        try {
            return AzureEmbeddedSynthesizer::Speak(params);
        } catch (const std::exception& e) {
            LogError("Exception in ProviderSpeak: %s", e.what());
            return false;
        } catch (...) {
            LogError("Unknown exception in ProviderSpeak");
            return false;
        }
    }
    
    // Export for SapiManager to trigger JSON generation
    __declspec(dllexport) bool __stdcall ProviderGenerateManifest(const wchar_t* outputDir)
    {
        try {
            if (!outputDir) return false;
            return VoiceManager::GenerateVoiceManifest(outputDir);
        } catch (...) {
            LogError("Unknown exception in ProviderGenerateManifest");
            return false;
        }
    }
}
