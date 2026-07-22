# ModernSapiAdapter

A high-performance, language-agnostic **SAPI 5 Speech Adapter ecosystem** for modern Windows environments. 

`ModernSapiAdapter` is designed as a spiritual successor to `NaturalVoiceSAPIAdapter`. It completely decouples the complex Windows SAPI 5 COM registration framework from text-to-speech synthesis backends (Providers) via a flat, unmanaged C-ABI boundary.

---

## Architecture Overview

The solution strictly separates COM plumbing, provider synthesis engines, and registry management into clean, independent components:

```
                  ┌──────────────────────────────────────────────┐
                  │          Windows Speech API (SAPI 5)          │
                  │        (JAWS, NVDA, Narrator, Apps)          │
                  └──────────────────────┬───────────────────────┘
                                         │ COM (ISpTTSEngine)
                                         ▼
                  ┌──────────────────────────────────────────────┐
                  │               CoreEngine.dll                 │
                  │      (C++20 COM Proxy Router Engine)         │
                  └──────────────────────┬───────────────────────┘
                                         │ Unmanaged C-ABI (provider_abi.h)
                                         ▼
                  ┌──────────────────────────────────────────────┐
                  │               Provider DLLs                  │
                  │  (C# Native AOT, C++, Rust, ONNX, Piper, etc.)│
                  └──────────────────────────────────────────────┘
```

### Key Components

1. **`CoreEngine` (`CoreEngine.dll`)**
   - Written in ISO C++20.
   - Implements mandatory SAPI 5 COM interfaces (`ISpTTSEngine`, `ISpObjectWithToken`).
   - Maps SAPI `SPVTEXTFRAG` text chains into clean UTF-16 fragment arrays (`ProviderSpeechFragment`).
   - Manages low-latency cancellation polling (`SPVES_ABORT` cutoffs in <20ms).
   - Routes PCM audio bytes and speech metadata events (`SPEVENT`) back to SAPI 5.

2. **Shared Provider ABI (`include/provider_abi.h`)**
   - Flat, C-compatible binary contract using `#pragma pack(push, 8)`.
   - Language-agnostic: Supports C# Native AOT, C++, Rust, Zig, Go, or any language compiling to native unmanaged code.
   - Uses zero-GC function pointers (`PFN_AUDIO_CALLBACK`, `PFN_METADATA_CALLBACK`) for streaming audio and word boundary tracking.

3. **`MockProvider` (`MockProvider.dll`)**
   - Lightweight reference provider implementation for verification, integration testing, and benchmarks.

4. **`CoreEngine.Tests`**
   - Google Test unit and integration suite covering COM interface initialization, ABI parameter mapping, and speech worker threads.

---

## Provider Manifest Specification (`<ProviderName>_voices.json`)

To eliminate complex memory marshaling during voice registration and avoid the freeze risks of virtual registry key hacks, providers export a static voice manifest file: `<ProviderName>_voices.json`.

### Manifest Flow
1. **Generation:** The provider DLL exports a function or command to generate its `<ProviderName>_voices.json` file on disk.
2. **Management App Discovery:** The Management Desktop App scans provider directories, reads the static JSON manifests, and presents available voices to the user.
3. **Deterministic Token Registration:** The Management Desktop App writes clean, physical SAPI 5 voice tokens directly to the Windows Registry (`HKLM\SOFTWARE\Microsoft\Speech\Voices\Tokens`).

### Sample Manifest & Config Structure

#### 1. `<ProviderName>_voices.json` (Provider Manifest - Read Only)
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

#### 2. `<ProviderName>_config.json` (Provider-Specific Config - Read/Write)
Stores user voice toggles, custom aliases, decryption keys, and custom voice directories. Storing settings per-provider eliminates the need for a global central `config.json` file.

##### Dual-Tier Configuration Precedence Model
Providers implement a strict multi-tier configuration hierarchy:
1. **Level 1 — Machine-Wide Baseline:** Located alongside the provider DLL module in the installation directory (`<ModuleDir>\<ProviderName>_config.json`). Provides system-level default settings and machine-wide voice path registrations.
2. **Level 2 — User-Wide Override (Highest Priority):** Located in `%LOCALAPPDATA%\ModernSapiAdapter\Config\<ProviderName>_config.json`. Allows per-user customization without requiring elevated administrator privileges.

##### Config Merging & Overriding Rules
- **Scalar Settings (`DecryptionKey`, `Region`, `EncryptedApiKey`):** User-Wide setting overrides Machine-Wide setting if defined.
- **Array Settings (`ExtraVoicePaths`):** Machine-Wide and User-Wide paths are merged and deduplicated, allowing system-wide and user-specific voice directories to be active simultaneously.
- **Voice Toggles (`voicesConfig`):** Per-voice user settings (`Enabled`, `CustomAlias`) override machine defaults per voice ID.

```json
{
  "providerWideConfig": {
    "DecryptionKey": "<optional_key_here>",
    "ExtraVoicePaths": "C:\\CustomVoices,D:\\UnpackedVoices"
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

- **Operating System:** Windows 11 exclusively (64-bit).
- **Architectures:** `x64`, `ARM64` (32-bit `Win32` targets are unsupported).
- **Standards:** ISO C++20 for native components; .NET 8+ for managed/Native AOT components.
- **Build Output:** Centralized per-project under `bin/$(MSBuildProjectName)/$(Platform)/$(Configuration)/` and `build/$(MSBuildProjectName)/$(Platform)/$(Configuration)/`.

---

## Repository Structure

```
ModernSapiAdapter/
├── Directory.Build.props    # Centralized MSBuild output configuration
├── ModernSapiAdapter.slnx   # Visual Studio Solution File (x64 / ARM64)
├── AGENTS.md                # Solution Constitution & Engineering Guidelines
├── include/
│   ├── provider_abi.h       # Shared C-ABI Header (Doxygen documented)
│   └── ProviderAbi.cs       # Shared C# P/Invoke ABI mirror
├── CoreEngine/              # SAPI 5 COM Router DLL project
├── CoreEngine.Tests/        # CoreEngine Google Test suite
├── MockProvider/            # Reference C-ABI Provider DLL
├── SapiManager/             # WPF Management & Voice Registration Suite (.NET 8)
├── SapiSsmlParser.Cpp/      # W3C SSML Parser C++20 Static Library
└── SapiSsmlParser.Cpp.Tests/ # SapiSsmlParser.Cpp Google Test suite
```

---

## Building the Solution

1. Open `ModernSapiAdapter.slnx` in **Visual Studio 2026 / latest**.
2. Select **`x64`** or **`ARM64`** platform and **`Debug`** or **`Release`** configuration.
3. Build the solution (`Ctrl+Shift+B`). Outputs will be generated in `bin/` and `build/`.
