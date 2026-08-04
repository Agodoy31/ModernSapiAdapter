# Windows 11 SAPI 5 Registry Token Mapping

The **Modern SAPI Adapter** exposes modern out-of-process Text-To-Speech (TTS) engines to legacy Windows Speech API (SAPI 5) applications by bridging SAPI 5 object tokens (`ISpObjectToken`) to external standalone provider executables.

This document details the registry key structures, custom attribute schemas, CLSID registration requirements, and programmatic binding mechanics used by `SapiManager` and `CoreEngine`.

---

## Module & Environment Requirements

| Requirement | Value |
| :--- | :--- |
| **Target Platform** | Windows 11 (x64, ARM64) |
| **Registry Hive Path** | `HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Speech\Voices\Tokens` |
| **CoreEngine CLSID** | `{91CD243C-63F7-441F-AE2F-45057005CB6D}` |
| **Management Application** | `SapiManager` (Windows C# management tool) |
| **COM Proxy Component** | `CoreEngine.dll` (`CSapiEngine` COM class) |

---

## Registry Key Layout & Token Specifications

Windows SAPI 5 enumerates installed speech voices by querying subkeys under the standard voice registry hive:

```text
HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Speech\Voices\Tokens\
    └── <UniqueVoiceIdentifier>\
        ├── (Default) = "Display Voice Name"
        ├── CLSID = "{91CD243C-63F7-441F-AE2F-45057005CB6D}"
        ├── ProviderExecutablePath = "C:\Path\To\Provider.exe"
        ├── ProviderPipeName = "mock_provider"
        ├── VoiceId = "mock_voice_en_us_1"
        └── Attributes\
            ├── Name = "Modern Mock Voice"
            ├── Gender = "Female"
            ├── Age = "Adult"
            ├── Language = "409"
            └── Vendor = "ModernSapiAdapter"
```

### Value Definitions

| Registry Value | Data Type | Requirement | Description |
| :--- | :--- | :--- | :--- |
| **`CLSID`** | `REG_SZ` | **Mandatory** | Must be set to `{91CD243C-63F7-441F-AE2F-45057005CB6D}`. Directs Windows SAPI 5 to load `CoreEngine.dll` as the COM engine proxy. |
| **`ProviderExecutablePath`** | `REG_SZ` | **Mandatory** | Absolute file path to the external provider executable. Used by `CoreEngine` to spawn the provider process via `CreateProcessW` if the named pipes are not active. |
| **`ProviderPipeName`** | `REG_SZ` | **Mandatory** | Base pipe name chosen by the provider. Used by `CoreEngine` to assemble target control and audio pipe paths. |
| **`VoiceId`** | `REG_SZ` | **Optional** | Provider-internal voice identifier. Transmitted in `sapi_speak` JSON payloads to select specific voice models within multi-voice provider executables. |

> [!IMPORTANT]
> The `CLSID` registry string `{91CD243C-63F7-441F-AE2F-45057005CB6D}` is strictly coupled between `CoreEngine.dll` (`CLSID_SapiEngine` in `dllmain.cpp`) and `SapiManager` (`RegistryManager.cs`). Updating this GUID in C++ code without updating `SapiManager` will cause SAPI 5 instantiation to fail with `REGDB_E_CLASSNOTREG`.

---

## Architectural Rationale & Binding Mechanics

### Why `ProviderPipeName` Is Stored in the Registry

External TTS providers are completely independent, standalone executables that define and own their own Named Pipe server endpoints upon installation. 

By storing `ProviderPipeName` inside the SAPI 5 registry token:
1. `CoreEngine` reads `ProviderPipeName` directly from `ISpObjectToken::GetStringValue(L"ProviderPipeName", &pszPipe)`.
2. `CoreEngine` constructs the secure, user-isolated pipe paths:
   - **Control Pipe:** `\\.\pipe\[ProviderPipeName]\[UserSID]\control`
   - **Audio Pipe:** `\\.\pipe\[ProviderPipeName]\[UserSID]\audio`
3. If the pipe is not active, `CoreEngine` launches `CreateProcessW(ProviderExecutablePath)`. This preserves strict decoupling—the provider executable manages its own pipe creation and configuration.

### Single-Proxy Architecture

Instead of registering separate COM DLLs for every third-party TTS engine, `ModernSapiAdapter` uses `CoreEngine.dll` as a single, unified COM proxy:

```text
[SAPI 5 Client]
       │
       ▼ (CoCreateInstance via CLSID)
[CoreEngine.dll] ── (Reads ISpObjectToken)
       │
       ├── Reads ProviderExecutablePath & ProviderPipeName
       ▼
[Named Pipe IPC] ──> [External Provider Executable]
```

When a screen reader or SAPI application selects a voice, Windows SAPI 5 instantiates `CoreEngine.dll` and invokes `ISpObjectWithToken::SetObjectToken()`, passing the `ISpObjectToken` pointer. `CSapiEngine` extracts `ProviderExecutablePath` and `ProviderPipeName` from the token interface to bind to the appropriate provider process dynamically.

---

## 64-Bit Registry Redirection (WOW64)

`ModernSapiAdapter` targets **Windows 11 64-bit exclusively** (`x64` and `ARM64`). 

> [!NOTE]
> 32-bit (`x86`) compilation and `WOW6432Node` registry paths are not available. SAPI 5 tokens must be written directly to the native 64-bit registry hive (`HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Speech\Voices\Tokens`).

---

## SapiManager Responsibility

The `SapiManager` desktop application acts as the Windows configuration GUI for managing SAPI tokens:
- **Token Registration:** Creates registry subkeys under `HKLM\SOFTWARE\Microsoft\Speech\Voices\Tokens\`, assigning `CLSID`, `ProviderExecutablePath`, `ProviderPipeName`, and `VoiceId`.
- **Token Enumeration & Testing:** Queries existing SAPI tokens to verify `CoreEngine.dll` COM registration and pipe accessibility.
- **Token Unregistration:** Deletes subkeys cleanly when removing a provider mapping.
