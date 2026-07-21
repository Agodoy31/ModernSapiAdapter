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
        // We will push a fake audio block per fragment
        uint8_t fakeAudio[1024] = { 0 };

        for (uint32_t i = 0; i < params->FragmentCount; ++i)
        {
            const ProviderSpeechFragment& frag = params->Fragments[i];

            // Check cancellation flag
            if (params->pAbortFlag && *params->pAbortFlag)
            {
                return false;
            }

            if (frag.Action == PROVIDER_ACTION_BOOKMARK)
            {
                // Push a fake metadata event for bookmark
                if (params->MetaCallback)
                {
                    ProviderSpeechEvent ev = {};
                    ev.EventType = PROVIDER_EVENT_BOOKMARK;
                    ev.TextOffset = frag.OriginalOffset;
                    ev.TextLength = frag.TextLength;
                    ev.AudioByteOffset = i * sizeof(fakeAudio);
                    params->MetaCallback(&ev, params->UserContext);
                }
                continue;
            }

            // Push fake audio via callback
            if (params->AudioCallback)
            {
                bool continueSynth = params->AudioCallback(fakeAudio, sizeof(fakeAudio), params->UserContext);
                if (!continueSynth) return false;
            }

            // Push a fake metadata event for speech
            if (params->MetaCallback)
            {
                ProviderSpeechEvent ev = {};
                ev.EventType = PROVIDER_EVENT_WORD_BOUNDARY;
                ev.TextOffset = frag.OriginalOffset;
                ev.TextLength = frag.TextLength;
                ev.AudioByteOffset = (i + 1) * sizeof(fakeAudio); // After this audio block
                params->MetaCallback(&ev, params->UserContext);
            }

            // Yield slightly to simulate synthesis delay
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        return true;
    }

}
