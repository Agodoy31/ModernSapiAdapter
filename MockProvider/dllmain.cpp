#include "pch.h"
#define PROVIDER_EXPORTS


BOOL APIENTRY DllMain(HMODULE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
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
        if (!params) return false;

        // Mock synthesis loop
        // We will push 5 blocks of fake audio data
        uint8_t fakeAudio[1024] = { 0 };

        for (int i = 0; i < 5; ++i)
        {
            // Check cancellation flag
            if (params->pAbortFlag && *params->pAbortFlag)
            {
                return false;
            }

            // Push fake audio via callback
            if (params->AudioCallback)
            {
                bool continueSynth = params->AudioCallback(fakeAudio, sizeof(fakeAudio), params->UserContext);
                if (!continueSynth) return false;
            }

            // Push a fake metadata event
            if (params->MetaCallback)
            {
                ProviderSpeechEvent ev = {};
                ev.EventType = PROVIDER_EVENT_WORD_BOUNDARY;
                ev.TextOffset = 0;
                ev.TextLength = 5;
                ev.AudioByteOffset = i * sizeof(fakeAudio);
                params->MetaCallback(&ev, params->UserContext);
            }

            // Yield slightly to simulate synthesis delay
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        return true;
    }

}
