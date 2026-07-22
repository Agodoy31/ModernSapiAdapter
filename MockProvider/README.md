# MockProvider

## Overview
`MockProvider` is a lightweight C++ testing module designed to validate the `CoreEngine` SAPI 5 proxy router. It implements the ABI defined in `provider_abi.h` and acts as a dummy speech synthesizer that can generate fixed audio buffers or simulate processing delays.

## Target Platform
- Operating System: Windows 11 64-bit (may work on Windows 10)
- Language: C++20
- Toolset: MSVC v145 (Visual Studio 2026)

## Architecture & Responsibilities
- ABI Compliance: Implements all mandatory functions exported from the DLL (`GetProviderAbiVersion`, `GetProviderAudioFormat`, `ProviderSpeak`).
- Audio Format Simulation: Hardcodes a 24kHz 16-bit Mono audio format to test the `CoreEngine`'s ability to negotiate output formats dynamically with SAPI.
- Execution Delay: Simulates a long-running synthesis task via a simple thread sleep, checking the `pAbortFlag` at each iteration.

## Build Notes
- The project generates `MockProvider.dll`, which is dynamically loaded by `CoreEngine.dll` for unit tests.
