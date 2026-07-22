# AzureEmbeddedSpeechProvider

## Project Overview
This project is a C++ plugin designed to bridge Windows' SAPI 5 with the Azure Embedded Speech SDK supporting custom neural voice / enterprise offline models. It acts purely as an unmanaged synthesizer module and does not implement any COM objects itself, but rather relies on the ModernSapiAdapter ecosystem.

## Architectural Constraints
- Language: C++20
- Target OS: Windows 11 64-bit (may work on Windows 10).

## External Dependencies
- Azure Cognitive Services Speech SDK
- C++/WinRT
- nlohmann_json

## Architecture & Lifecycle

- Discovery & Manifest Generation: The `VoiceManager` class evaluates Machine-Wide and User-Wide `ExtraVoicePaths` via `ConfigParser`, scans for installed MSIX packages via WinRT, queries voice metadata via `EmbeddedSpeechConfig::FromPaths`, and writes the `AzureEmbeddedSpeechProvider_voices.json` manifest.
- Configuration Precedence: The `ConfigParser` class loads configuration settings using a dual-tier hierarchy:
  - Machine-Wide Baseline: `<ModuleDir>\AzureEmbeddedSpeechProvider_config.json`
  - User-Wide Override: `%LOCALAPPDATA%\ModernSapiAdapter\Config\AzureEmbeddedSpeechProvider_config.json`
  - keys (`DecryptionKey`) in User-Wide config override Machine-Wide config. Array keys (`ExtraVoicePaths`) are merged and deduplicated.
- Initialization: The `SynthesizerPool` executes `ConfigParser::LoadMergedConfig()`, builds the global Azure `EmbeddedSpeechConfig`, and injects the decryption key.
- Synthesis Engine: `AzureEmbeddedSynthesizer` instantiates request-scoped `SpeechSynthesizer` instances on the fly using `CreatePushStream`. It translates Azure's 100-nanosecond tick offsets into raw SAPI audio byte offsets (24kHz, 16-bit Mono = `(Ticks * 48) / 10000`).

## Build Instructions
- The project uses Precompiled Headers.
- MSBuild will automatically statically link `SapiSsmlParser.Cpp.lib` and `PcmAudioUtils.lib` during the Linker phase into the resulting `AzureEmbeddedSpeechProvider.dll`.