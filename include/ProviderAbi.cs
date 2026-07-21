/// <summary>
/// Shared ABI contract between the SAPI 5 C++ wrapper (CoreEngine) and .NET Native AOT provider DLLs.
/// This is the C# equivalent of <c>provider_abi.h</c> and must be kept in sync with it.
/// </summary>
/// <remarks>
/// All structs use <see cref="System.Runtime.InteropServices.StructLayoutAttribute"/> with
/// <see cref="LayoutKind.Sequential"/> and Pack=8 to match the C++ <c>#pragma pack(push, 8)</c> layout.
/// Provider authors: copy this file into your .NET Native AOT project and implement the three
/// required exports (<c>GetProviderAbiVersion</c>, <c>GetProviderAudioFormat</c>, <c>ProviderSpeak</c>)
/// using <see cref="System.Runtime.InteropServices.UnmanagedCallersOnlyAttribute"/>.
/// </remarks>

using System.Runtime.InteropServices;

namespace ModernSapiAdapter.Abi;

#region Constants

/// <summary>
/// ABI constants mirroring the #define values in provider_abi.h.
/// </summary>
public static class ProviderAbiConstants
{
    // Speech tracking event types
    public const uint EventWordBoundary     = 1;
    public const uint EventSentenceBoundary = 2;
    public const uint EventBookmark         = 3;

    // Speech action types
    public const uint ActionSpeak    = 0;
    public const uint ActionSpellOut = 1;
    public const uint ActionPronounce = 2;
    public const uint ActionBookmark = 3;

    /// <summary>
    /// The current ABI contract version. Must match PROVIDER_ABI_VERSION in provider_abi.h.
    /// </summary>
    public const uint AbiVersion = 2;
}

#endregion

#region Structs

/// <summary>
/// Describes the native PCM audio output format of the provider.
/// Mirrors <c>ProviderAudioFormat</c> in provider_abi.h. Size: 16 bytes.
/// </summary>
[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct ProviderAudioFormat
{
    /// <summary>Sample rate in Hz (e.g. 16000, 24000, 44100, 48000).</summary>
    public uint SampleRate;

    /// <summary>Bits per sample (e.g. 16, 24, 32).</summary>
    public ushort BitsPerSample;

    /// <summary>Number of audio channels (1 = Mono, 2 = Stereo).</summary>
    public ushort Channels;

    /// <summary>Explicit alignment padding. Must be 0.</summary>
    public ulong Reserved;
}

/// <summary>
/// Speech tracking event pushed to the C++ wrapper via MetaCallback.
/// Mirrors <c>ProviderSpeechEvent</c> in provider_abi.h. Size: 24 bytes.
/// </summary>
[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct ProviderSpeechEvent
{
    /// <summary>Event classification (see <see cref="ProviderAbiConstants"/>).</summary>
    public uint EventType;

    /// <summary>Character index (Original SAPI ulTextSrcOffset).</summary>
    public uint TextOffset;

    /// <summary>Length of the active text span in char16 units.</summary>
    public uint TextLength;

    /// <summary>Explicit alignment padding. Must be 0.</summary>
    public uint Reserved;

    /// <summary>Accumulated PCM byte position linked to this event.</summary>
    public ulong AudioByteOffset;
}

/// <summary>
/// Represents a single speech segment, bookmark, or synthesis command.
/// Mirrors <c>ProviderSpeechFragment</c> in provider_abi.h. Size: 32 bytes.
/// </summary>
[StructLayout(LayoutKind.Sequential, Pack = 8)]
public unsafe struct ProviderSpeechFragment
{
    /// <summary>Fragment text (or Bookmark name).</summary>
    public char* Text;

    /// <summary>Length of the text in char16 units.</summary>
    public uint TextLength;

    /// <summary>The original SAPI ulTextSrcOffset for this fragment.</summary>
    public uint OriginalOffset;

    /// <summary>Speech mode (see <see cref="ProviderAbiConstants"/>).</summary>
    public uint Action;

    /// <summary>Absolute volume level (0.0 to 100.0).</summary>
    public float Volume;

    /// <summary>Rate adjustment.</summary>
    public float Rate;

    /// <summary>Pitch offset.</summary>
    public float Pitch;
}

/// <summary>
/// Consolidated parameter block passed to ProviderSpeak.
/// Mirrors <c>ProviderSpeakParams</c> in provider_abi.h. Size: 56 bytes.
/// </summary>
/// <remarks>
/// Memory contract: All pointer targets are owned by the C++ wrapper and are
/// guaranteed valid only for the synchronous duration of the ProviderSpeak call.
/// </remarks>
[StructLayout(LayoutKind.Sequential, Pack = 8)]
public unsafe struct ProviderSpeakParams
{
    /// <summary>Array of speech fragments.</summary>
    public ProviderSpeechFragment* Fragments;

    /// <summary>Voice identifier string.</summary>
    public char* VoiceModel;

    /// <summary>Non-zero signals immediate termination.</summary>
    public uint* PAbortFlag;

    /// <summary>Opaque handle forwarded into callbacks.</summary>
    public nint UserContext;

    /// <summary>PCM audio output handler function pointer.</summary>
    public nint AudioCallback;

    /// <summary>Speech event output handler function pointer.</summary>
    public nint MetaCallback;

    /// <summary>ABI struct version (must equal <see cref="ProviderAbiConstants.AbiVersion"/>).</summary>
    public uint ContractVersion;

    /// <summary>Number of fragments in array.</summary>
    public uint FragmentCount;
}

#endregion

#region Callback Delegates

/// <summary>
/// Streams raw PCM audio bytes back to the C++ wrapper.
/// Mirrors <c>PFN_AUDIO_CALLBACK</c> in provider_abi.h.
/// </summary>
/// <param name="pAudioBytes">Pointer to raw PCM audio byte buffer.</param>
/// <param name="byteCount">Number of bytes in the buffer.</param>
/// <param name="ctx">Opaque user context handle.</param>
/// <returns><c>true</c> to continue synthesis; <c>false</c> to immediately abort.</returns>
[UnmanagedFunctionPointer(CallingConvention.StdCall)]
public unsafe delegate bool AudioCallback(byte* pAudioBytes, uint byteCount, nint ctx);

/// <summary>
/// Pushes a speech tracking event for SAPI SPEVENT translation.
/// Mirrors <c>PFN_METADATA_CALLBACK</c> in provider_abi.h.
/// </summary>
/// <param name="pEvent">Pointer to the populated event structure.</param>
/// <param name="ctx">Opaque user context handle.</param>
[UnmanagedFunctionPointer(CallingConvention.StdCall)]
public unsafe delegate void MetadataCallback(ProviderSpeechEvent* pEvent, nint ctx);

#endregion
