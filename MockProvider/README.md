# MockProvider

## Overview
`MockProvider` is a lightweight, unmanaged C++ testing module designed to validate the `CoreEngine` SAPI 5 proxy router. It implements the C-ABI boundary defined in `provider_abi.h` and acts as a dummy speech synthesizer that can generate fixed audio buffers or simulate processing delays.

## Target Platform & Standards
- **Operating System:** Windows 11 exclusively (64-bit)
- **Architectures:** `x64`, `ARM64` (32-bit `Win32` targets are explicitly unsupported)
- **Language Standard:** ISO C++20 (`stdcpp20`)
- **Toolset:** MSVC v145 (Visual Studio 2026 / latest)

## Architecture & Responsibilities
1. **ABI Compliance:** Implements all mandatory functions exported from the DLL (`ProviderSpeak`, `GetProviderAudioFormat`, `EnumerateVoices`, `InitProvider`, `CleanupProvider`).
2. **Audio Format Simulation:** Hardcodes a 24kHz 16-bit Mono audio format to test the `CoreEngine`'s ability to negotiate output formats dynamically with SAPI.
3. **Execution Delay:** Simulates a long-running synthesis task via a simple thread sleep, checking the `pAbortFlag` at each iteration to validate the `<20ms` abort polling threshold.

## Build Notes
- Compilation is performed manually in Visual Studio under `x64` or `ARM64` configurations.
- The project generates `MockProvider.dll`, which is dynamically loaded by `CoreEngine.dll` for unit testing and runtime validation.

## Library Dependencies
- The `MockProvider` relies strictly on the C++ Standard Library (`<thread>`, `<chrono>`) and standard Windows APIs.
- It purposely avoids COM, SAPI, and C++/WinRT headers to guarantee a clean, unmanaged C-ABI boundary execution.
