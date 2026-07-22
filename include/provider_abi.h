/**
 * @file provider_abi.h
 * @brief Shared ABI contract between the SAPI 5 C++ wrapper (CoreEngine) and provider DLLs.
 */

#ifndef PROVIDER_ABI_H
#define PROVIDER_ABI_H
#pragma once

#include <cstdint>



#define PROVIDER_EVENT_WORD_BOUNDARY      1
#define PROVIDER_EVENT_SENTENCE_BOUNDARY  2
#define PROVIDER_EVENT_BOOKMARK           3

#define PROVIDER_ACTION_SPEAK             0
#define PROVIDER_ACTION_SPELL_OUT         1
#define PROVIDER_ACTION_PRONOUNCE         2
#define PROVIDER_ACTION_BOOKMARK          3

#define PROVIDER_ABI_VERSION              3



#pragma pack(push, 8)

/**
 * @brief Describes the native PCM audio output format of the provider.
 * @note Struct size is exactly 16 bytes with zero implicit padding gaps.
 */
struct ProviderAudioFormat {
    uint32_t SampleRate;       ///< Sample rate in Hz (e.g. 16000, 24000, 44100, 48000)
    uint16_t BitsPerSample;    ///< Bits per sample (e.g. 16, 24, 32)
    uint16_t Channels;         ///< Number of audio channels (e.g. 1 = Mono, 2 = Stereo)
    uint64_t Reserved;         ///< Explicit alignment padding (must be 0)
};

/**
 * @brief Speech tracking event pushed to the C++ wrapper via MetaCallback.
 * @note Struct size is exactly 32 bytes with zero implicit padding gaps.
 */
struct ProviderSpeechEvent {
    uint32_t EventType;        ///< Event classification (PROVIDER_EVENT_*)
    uint32_t TextOffset;       ///< Character index (Original SAPI ulTextSrcOffset)
    uint32_t TextLength;       ///< Length of the active text span in char16_t units
    uint32_t Reserved;         ///< Explicit alignment padding (must be 0)
    uint64_t AudioByteOffset;  ///< Accumulated PCM byte position linked to this event
    const char16_t* StringData; ///< Optional string data (e.g. Bookmark name)
};



/**
 * @brief Streams raw PCM audio bytes back to the C++ wrapper.
 * @param[in] pAudioBytes Pointer to raw PCM audio byte buffer.
 * @param[in] byteCount Number of bytes in pAudioBytes buffer.
 * @param[in] ctx Opaque user context handle.
 * @return True to continue synthesis; false to immediately abort.
 */
typedef bool (__stdcall* PFN_AUDIO_CALLBACK)(
    const uint8_t* pAudioBytes,
    uint32_t       byteCount,
    void*          ctx
);

/**
 * @brief Pushes a speech tracking event for SAPI SPEVENT translation.
 * @param[in] pEvent Pointer to the populated event structure.
 * @param[in] ctx Opaque user context handle.
 */
typedef void (__stdcall* PFN_METADATA_CALLBACK)(
    const ProviderSpeechEvent* pEvent,
    void*                      ctx
);

/**
 * @brief Represents a single speech segment, bookmark, or synthesis command.
 * @note Struct size is exactly 32 bytes with zero implicit padding gaps.
 */
struct ProviderSpeechFragment {
    const char16_t* Text;            ///< Fragment text (or Bookmark name)
    uint32_t        TextLength;      ///< Length of the text in char16_t units
    uint32_t        OriginalOffset;  ///< The original SAPI ulTextSrcOffset for this fragment
    uint32_t        Action;          ///< Speech mode (PROVIDER_ACTION_*)
    float           Volume;          ///< Absolute volume level (0.0 to 100.0)
    float           Rate;            ///< Rate adjustment
    float           Pitch;           ///< Pitch offset
};

/**
 * @brief Consolidated parameter block passed to ProviderSpeak.
 * @note Memory contract: All pointer targets are owned by the C++ wrapper and are
 *       guaranteed valid only for the synchronous duration of the ProviderSpeak call.
 *       Struct size is exactly 56 bytes.
 */
struct ProviderSpeakParams {
    const ProviderSpeechFragment* Fragments;      ///< Array of speech fragments
    const char16_t*               VoiceModel;     ///< Voice identifier string
    const volatile uint32_t*      pAbortFlag;     ///< Non-zero signals immediate termination
    void*                         UserContext;    ///< Opaque handle forwarded into callbacks
    PFN_AUDIO_CALLBACK            AudioCallback;  ///< PCM audio output handler
    PFN_METADATA_CALLBACK         MetaCallback;   ///< Speech event output handler

    uint32_t ContractVersion;                     ///< ABI struct version (PROVIDER_ABI_VERSION)
    uint32_t FragmentCount;                       ///< Number of fragments in array
};

#pragma pack(pop)



typedef uint32_t (__stdcall* PFN_GET_PROVIDER_ABI_VERSION)(void);
typedef bool     (__stdcall* PFN_GET_PROVIDER_AUDIO_FORMAT)(ProviderAudioFormat* format);
typedef bool     (__stdcall* PFN_PROVIDER_SPEAK)(const ProviderSpeakParams* params);

#ifdef PROVIDER_EXPORTS
extern "C" {
    /** @brief Returns the provider's compiled ABI version. */
    __declspec(dllexport) uint32_t __stdcall GetProviderAbiVersion(void);

    /** @brief Queries the provider's native PCM audio format. */
    __declspec(dllexport) bool     __stdcall GetProviderAudioFormat(ProviderAudioFormat* format);

    /** @brief Synthesizes speech synchronously using the provided parameters block. */
    __declspec(dllexport) bool     __stdcall ProviderSpeak(const ProviderSpeakParams* params);
}
#endif

#endif // PROVIDER_ABI_H
