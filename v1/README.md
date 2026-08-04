# ModernSapiAdapter

A high-performance, language-agnostic SAPI 5 Speech Adapter ecosystem for modern Windows environments. 

`ModernSapiAdapter` is designed as a spiritual successor to  monolithic Sapi5 engines by completely decoupling the complex Windows SAPI 5 COM registration framework from text-to-speech synthesis backends via a flat C-style ABI contract.

---

## Architecture Overview

The solution strictly separates COM plumbing, provider synthesis engines, and registry management into clean, independent components:

```
                  ┌──────────────────────────────────────────────┐
                  │          Windows Speech API (SAPI 5)          │
                  └──────────────────────┬───────────────────────┘
                                         │ COM (ISpTTSEngine)
                                         ▼
                  ┌──────────────────────────────────────────────┐
                  │               CoreEngine.dll                 │
                  │      (C++20 COM Proxy Router Engine)         │
                  └──────────────────────┬───────────────────────┘
                                         │ C-style ABI (provider_abi.h)
                                         ▼
                  ┌──────────────────────────────────────────────┐
                  │               Provider DLLs                  │
                  │  (C# Native AOT, C++, Rust, etc.)│
                  └──────────────────────────────────────────────┘
```

### Key Components

1. `CoreEngine` 
   - Written in C++20.
   - Implements mandatory SAPI 5 COM interfaces (`ISpTTSEngine`, `ISpObjectWithToken`).
   - Maps SAPI `SPVTEXTFRAG` text chains into clean UTF-16 fragment arrays (`ProviderSpeechFragment`).
   - Manages low-latency cancellation polling (`SPVES_ABORT` cutoffs).
   - Routes PCM audio bytes and speech metadata events (`SPEVENT`) back to SAPI 5.

2. Shared Provider Contract
   - C-compatible binary contract using
   - Language-agnostic: Supports C# Native AOT, C++, Rust, Zig, Go, or any language compiling to machine code.
   - Uses zero-GC function pointers (`PFN_AUDIO_CALLBACK`, `PFN_METADATA_CALLBACK`) for streaming audio and word boundary tracking.

3. `MockProvider`
   - Lightweight reference provider implementation for verification and unit testing.

4. `CoreEngine.Tests`
   - Google Test unit tests covering COM interface initialization, ABI parameter mapping, and speech worker threads.

---

## Provider Manifest Specification (`<ProviderName>_voices.json`)

To eliminate complex memory marshaling during voice registration and avoid the freeze risks of virtual registry key hacks, providers export a static voice manifest file: `<ProviderName>_voices.json`.

### Manifest Flow
- Generation: The provider DLL exports a function to generate its `<ProviderName>_voices.json` file on disk.
- Management App Discovery: The Management App scans provider directories, reads the static JSON manifests, and presents available voices to the user.
- Token Registration: The Management Desktop App writes SAPI 5 voice tokens directly to the Windows Registry (`HKLM\SOFTWARE\Microsoft\Speech\Voices\Tokens`).

### Sample Manifest & Config Structure

#### `<ProviderName>_voices.json` (Provider Manifest)
```json
{
  "providerName": "AzureTTS",
  "version": "1.0.0",
  "configSchema": [
    {
      "key": "Region",
      "type": "string",
      "displayName": "Azure Region",
      "description": "The region for your Azure Speech resource (e.g. eastus)."
    }
  ],
  "voices": [
    {
      "voiceId": "en-US-JennyNeural",
      "sapiAttributes": {
        "Language": "en-US",
        "Gender": "Female",
        "Age": "Adult",
        "Name": "Microsoft Jenny Online (Natural)",
        "Vendor": "Microsoft"
      }
    }
  ]
}
```

#### `<ProviderName>_config.json` (Provider-Specific Config)
Stores user voice toggles, custom aliases, decryption keys, and custom voice directories. Storing settings per-provider eliminates the need for a global central `config.json` file.

##### Dual-Tier Configuration Precedence Model
Providers may implement a multi-tier configuration hierarchy:
- Level 1: Machine-Wide Baseline: Located alongside the provider DLL module in the installation directory (`<ModuleDir>\<ProviderName>_config.json`). Provides system-level default settings and machine-wide voice settings.
- Level 2: User-Wide Override: Located in `%LOCALAPPDATA%\ModernSapiAdapter\Config\<ProviderName>_config.json`. Allows per-user customization without requiring elevated administrator privileges.

```json
{
  "providerWideConfig": {
    "Region": "<Azure_cognitive_services_key>"
  },
  "voicesConfig": {
    "en-US-JennyNeural": {
      "Enabled": true,
      "CustomAlias": "Jenny (Azure Cloud)"
    }
  }
}
```

---

## Target Environment & Guidelines

- Operating System: Windows 11 64-bit (may work on windows 10).
- Standards: C++20 for native components; .NET 8+ for C# components.
- Build Output: Centralized per-project under `bin/$(MSBuildProjectName)/$(Platform)/$(Configuration)/`.

---

## Repository Structure

```
ModernSapiAdapter/
├── Directory.Build.props    # Centralized MSBuild output configuration
├── ModernSapiAdapter.slnx   # Visual Studio Solution File
├── include/
│   ├── provider_abi.h       # Shared C-style contract
│   └── ProviderAbi.cs       # Shared C# contract mirror
├── CoreEngine/              # SAPI 5 COM Router DLL project
├── CoreEngine.Tests/        # CoreEngine Google Test suite
├── MockProvider/            # Reference C-ABI Provider DLL
├── PcmAudioUtils/           # Low-Latency PCM Trimming & Resampling C++20 Static Library
├── AzureEmbeddedSpeechProvider/ # Azure Embedded Neural Voice Provider DLL
├── SapiManager/             # WPF Management & Voice Registration Suite
├── SapiSsmlParser.Cpp/      # W3C SSML Parser C++20 Static Library
└── SapiSsmlParser.Cpp.Tests/ # SapiSsmlParser.Cpp Google Test suite
```

---
