# CoreEngine (SAPI 5 Proxy Router)

## Overview
`CoreEngine` is the unmanaged C++ COM proxy router for the ModernSapiAdapter solution. It implements the mandatory Windows SAPI 5 COM interfaces (`ISpTTSEngine`, `ISpObjectWithToken`, etc.) and acts as the gatekeeper between the Windows Speech API subsystem and external speech synthesis provider DLLs.

## Target Platform & Standards
- **Operating System:** Windows 11 exclusively (64-bit)
- **Architectures:** `x64`, `ARM64` (32-bit `Win32` targets are explicitly unsupported)
- **Language Standard:** ISO C++20 (`stdcpp20`)
- **Toolset:** MSVC v145 (Visual Studio 2026 / latest)

## Architecture & Responsibilities
1. **COM Registration & Routing:** Exposes SAPI 5 COM engine CLSID and implements SAPI 5 stream/token interfaces.
2. **Text Flattening:** Traverses SAPI 5 `SPVTEXTFRAG` linked lists into flat UTF-16 wide-character buffers (`char16_t*`).
3. **Dynamic Provider Loading:** Dynamically loads provider DLLs via `LoadLibraryW` / `GetProcAddress` and communicates via the unmanaged ABI contract defined in `$(SolutionDir)include/provider_abi.h`.
4. **Low-Latency Polling & Event Dispatch:** Monitors `ISpTTSEngineSite` actions for immediate `SPVES_ABORT` signals (<20ms cutoff) and maps provider tracking metadata into SAPI `SPEVENT` structures.
5. **Standard COM Exports:** Strictly utilizes `CoreEngine.def` alongside `STDAPI` endpoints (`DllGetClassObject`, `DllRegisterServer`, etc.) to properly expose the COM factory to `regsvr32` and `CoCreateInstance`.

## Build Notes
- Compilation is performed manually in Visual Studio under `x64` or `ARM64` configurations.
- Shared ABI contract header is located at `$(SolutionDir)include/provider_abi.h`.

## ABI Design & Implementation Plan
Based on an architectural deep dive, the `CoreEngine` and `provider_abi.h` are designed with the following principles to balance SAPI 5 compatibility with provider simplicity:

1. **Text Format & SSML Translation:** `CoreEngine` is responsible for parsing legacy SAPI XML and translating it into standard SSML (Speech Synthesis Markup Language) before passing it to the Provider. This ensures providers only need to parse standard SSML and can natively handle bookmarks and prosody.
2. **Audio/Meta Synchronization:** Providers must fire SAPI tracking events (`PROVIDER_EVENT_WORD_BOUNDARY`, etc.) before or exactly when the corresponding audio chunk is submitted to the CoreEngine.
3. **Format Negotiation:** The ABI defines `GetProviderAudioFormat` so providers can expose their native PCM format (Sample Rate, Bit Depth, Channels) to the `CoreEngine`, which relays it to SAPI.
4. **Voice Discovery:** The ABI defines a callback-based `EnumerateVoices` function, allowing an external installer or the CoreEngine to discover voices and write the appropriate SAPI 5 Object Tokens.
5. **Execution Model & Threading:** `CoreEngine` encapsulates all threading complexity. It spawns a background worker thread to call the blocking `ProviderSpeak` function, while the main SAPI thread pumps `GetActions()` to detect `SPVES_ABORT` signals. This keeps provider implementations synchronous and trivial.
6. **Skip Handling:** `CoreEngine` handles all SAPI `Skip` requests. It aborts the current `ProviderSpeak` call, advances its internal text pointers, and invokes a new `ProviderSpeak` session, keeping the provider ignorant of SAPI's skipping mechanisms.

## Library Dependencies
- **C++/WinRT**: Used via `<winrt/base.h>` and `winrt::implements` to author the `ISpTTSEngine` and `ISpObjectWithToken` COM interfaces cleanly without ATL macros.
- **WIL (Windows Implementation Libraries)**: Used via `<wil/cppwinrt.h>` to bridge Win32 errors into C++/WinRT exceptions. We will heavily utilize `wil::com_ptr` (or `winrt::com_ptr`), `wil::unique_hmodule`, and `THROW_IF_FAILED` for robust resource management.

## C++ Class Separation

To avoid a monolithic nightmare and ensure safe thread lifecycles when SAPI destroys the COM object, we will separate concerns into three distinct primary classes:

### 1. `CSapiEngine` (COM Layer)
- **Role**: Implements the SAPI 5 COM interfaces (`ISpTTSEngine`, `ISpObjectWithToken`).
- **Responsibilities**: Handles SAPI registry token parsing, receives the `Speak()` command, and instantiates the `SpeechWorker`.
- **Files**: `SapiEngine.h`, `SapiEngine.cpp`

### 2. `ProviderWrapper` (ABI Layer)
- **Role**: Manages the lifecycle and function pointers of the loaded Provider DLL.
- **Responsibilities**: Calls `LoadLibrary`, extracts ABI function pointers (e.g., `ProviderSpeak`, `GetProviderAudioFormat`), and translates C-ABI data structures safely back and forth.
- **Files**: `ProviderWrapper.h`, `ProviderWrapper.cpp`

### 3. `SpeechWorker` (Execution Layer)
- **Role**: Encapsulates the background synthesis thread.
- **Responsibilities**: Executes `ProviderSpeak` on a background thread. Manages the `pAbortFlag`. Intercepts the C-style `PFN_AUDIO_CALLBACK` and `PFN_METADATA_CALLBACK`, marshaling them into synchronous SAPI `ISpEventSink::AddEvents` and `ISpMMSysAudio::Write` calls.
- **Files**: `SpeechWorker.h`, `SpeechWorker.cpp`
