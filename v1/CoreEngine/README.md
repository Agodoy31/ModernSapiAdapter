# SAPI 5 Proxy Router

## Overview
`CoreEngine` is the C++ COM proxy router for the ModernSapiAdapter ecosystem. It implements the mandatory Windows SAPI 5 COM interfaces (`ISpTTSEngine`, `ISpObjectWithToken`, etc.) and acts as the bridge between the Windows Speech API subsystem and external speech synthesis provider DLLs.

## Target Platform & Standards
- Operating System: Windows 11 64-bit (may work on Windows 10)
- Language: C++20
- Toolset: MSVC v145 (Visual Studio 2026)

## Architecture & Responsibilities
- COM Registration & Routing: Exposes SAPI 5 COM engine CLSID and implements SAPI 5 stream/token interfaces.
- Fragment Array ABI: Traverses SAPI 5 `SPVTEXTFRAG` linked lists and converts them into an array of `ProviderSpeechFragment` structures, preserving SAPI metadata like original offsets and bookmark types.
- Dynamic Provider Loading: Dynamically loads provider DLLs via `LoadLibraryW` / `GetProcAddress` and communicates via the ABI contract defined in `include/provider_abi.h`.
- Low-Latency Polling & Event Dispatch: Monitors `ISpTTSEngineSite` actions for immediate `SPVES_ABORT` signals and maps provider tracking metadata into SAPI `SPEVENT` structures.

## Build Notes
- Shared ABI contract header is located at `$(SolutionDir)include/provider_abi.h`.

## ABI Design
The `CoreEngine` and `provider_abi.h` are designed with the following principles to balance SAPI 5 compatibility with provider simplicity:

- Fragment Format & State Extraction: `CoreEngine` is strictly language-agnostic and does not mutate the text buffer. It passes an array of `ProviderSpeechFragment` structures directly to the Provider and explicitly extracts playback state variables (Volume, Rate, Pitch) from SAPI's `SPVSTATE` struct to pass via the ABI. Providers are responsible for translating text into SSML if required by their respective backend engines, using the precise `OriginalOffset` provided to accurately fire SAPI events.
- Audio/Meta Synchronization: Providers must fire SAPI tracking events (`PROVIDER_EVENT_WORD_BOUNDARY`, etc.) before or exactly when the corresponding audio chunk is submitted to the CoreEngine.
- Format Negotiation: The ABI defines `GetProviderAudioFormat` so providers can expose their native PCM format (Sample Rate, Bit Depth, Channels) to the `CoreEngine`, which relays it to SAPI.
- Execution Model & Threading: `CoreEngine` encapsulates all threading complexity. It spawns a background worker thread to call the blocking `ProviderSpeak` function, while the main SAPI thread pumps `GetActions()` to detect `SPVES_ABORT` signals. This keeps provider implementations synchronous and trivial.
- Skip Handling: `CoreEngine` handles all SAPI `Skip` requests. It aborts the current `ProviderSpeak` call, advances its internal text pointers, and invokes a new `ProviderSpeak` session, keeping the provider ignorant of SAPI's skipping mechanisms.

## Library Dependencies
- C++/WinRT
- WIL (Windows Implementation Libraries)

## C++ Class Separation

To avoid a monolithic nightmare, three primary classes are implemented:

### 1. `CSapiEngine` (COM Layer)
- Implements the SAPI 5 COM interfaces (`ISpTTSEngine`, `ISpObjectWithToken`).
- Handles SAPI registry token parsing, receives the `Speak()` command, instantiates the `SpeechWorker`, and encapsulates `m_siteMutex` to guarantee thread-safe marshaling for `ISpTTSEngineSite` (`Write`, `AddEvents`, `GetActions`) when providers invoke callbacks on concurrent threads.
- Files: `SapiEngine.h`, `SapiEngine.cpp`

### 2. `ProviderWrapper` (ABI Layer)
- Manages the lifecycle and function pointers of the loaded Provider DLL.
- Calls `LoadLibrary`, extracts ABI function pointers (e.g., `ProviderSpeak`, `GetProviderAudioFormat`), and translates C-style data structures safely back and forth.
- Files: `ProviderWrapper.h`, `ProviderWrapper.cpp`

### 3. `SpeechWorker` (Execution Layer)
- Encapsulates the background synthesis thread.
- Executes `ProviderSpeak` on a background thread. Manages the `pAbortFlag`. Intercepts the C-style `PFN_AUDIO_CALLBACK` and `PFN_METADATA_CALLBACK`, marshaling them into synchronous SAPI `ISpEventSink::AddEvents` and `ISpMMSysAudio::Write` calls.
- Files: `SpeechWorker.h`, `SpeechWorker.cpp`
