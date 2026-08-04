# SapiManager Architecture & Subsystem Specification

The **SapiManager** application is the C# .NET 8 WPF management tool for the **Modern SAPI Adapter** ecosystem. It provides the administration interface, installation wizard, provider package ingestion engine, and SAPI 5 system registry token manager.

This specification details the application lifecycle, service architecture, registry token binding mechanics, IPC probing protocol, ZIP package installation pipeline, and end-to-end execution workflows.

---

## Module & Environment Requirements

| Requirement | Value |
| :--- | :--- |
| **Target Platform** | Windows 11 (x64, ARM64) |
| **Framework** | .NET 8 WPF |
| **Execution Level** | Administrator (`requireAdministrator` in `app.manifest`) |
| **Registry Hive Path** | `HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Speech\Voices\Tokens` |
| **CoreEngine CLSID** | `{91CD243C-63F7-441F-AE2F-45057005CB6D}` |
| **Default Install Directory** | `C:\Program Files\ModernSapiAdapter` |
| **Providers Directory** | `.\providers\<ProviderFolder>\` |

---

## Bootstrapping & Setup Mode Routing

The entry point (`App.xaml.cs`) manages application bootstrapping, target directory verification, and setup routing.

### Installation Location Verification

Upon startup, `App.xaml.cs` queries `RegistryManager.GetInstallLocation()` and compares it against `AppDomain.CurrentDomain.BaseDirectory`.

```text
                               +-----------------------------+
                               |     App.xaml.cs Startup     |
                               +-----------------------------+
                                              |
                                              v
                              +-------------------------------+
                              | Check Installed Version vs    |
                              | Current Assembly Version      |
                              +-------------------------------+
                                     /                 \
                                    /                   \
                   Installed Path Matches            Installed Path Differs
                    & Version Matches                  or Mismatched Version
                           /                                     \
                          v                                       v
               +----------------------+                +----------------------+
               |   MainWindow.xaml    |                | InstallerWindow.xaml |
               |  (Normal Dashboard)  |                |  (Install / Upgrade) |
               +----------------------+                +----------------------+
```

1. **Normal Mode**: If the executable is running from `C:\Program Files\ModernSapiAdapter` and versions match, `MainWindow` is launched.
2. **Setup / Upgrade Mode**: If running from an installer package or outside the target location, `InstallerWindow` is launched in `InstallerMode.Install` or `InstallerMode.Upgrade`.
3. **Uninstall Mode**: If launched with the `/uninstall` CLI flag, `InstallerWindow` is launched in `InstallerMode.Uninstall`.

> [!IMPORTANT]
> Because registry modifications under `HKEY_LOCAL_MACHINE` require elevated privileges, `SapiManager.exe` embeds an `app.manifest` specifying `requireAdministrator`.

---

## Core Service Subsystems

### SAPI 5 Registry Engine (`RegistryManager.cs`)

`RegistryManager` encapsulates all read and write operations to the Windows SAPI 5 voice registry hive.

#### Key Functions

- **`RegisterVoiceToken(...)`**: Creates or updates a voice token under `HKLM\SOFTWARE\Microsoft\Speech\Voices\Tokens\<TokenName>`. Sets the mandatory `CLSID` to `{91CD243C-63F7-441F-AE2F-45057005CB6D}`, `ProviderExecutablePath`, `ProviderPipeName`, optional `VoiceId`, and voice attributes (`Name`, `Gender`, `Age`, `Language`, `Vendor`).
- **`UnregisterVoiceToken(tokenName)`**: Deletes the specified voice token subkey from HKLM.
- **`IsVoiceTokenRegistered(tokenName)`**: Returns `true` if the voice token key exists in HKLM.
- **`RegisterUninstallInfo(...)`**: Registers uninstaller metadata under `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\ModernSapiAdapter` (`InstallLocation`, `DisplayVersion`, `UninstallString`).

---

### Provider Discovery Subsystem (`ProviderDiscovery.cs`)

`ProviderDiscovery` dynamically inspects external provider executables and their associated manifests without requiring pre-installed registry entries.

#### Discovery & Probing Sequence

1. Verifies that the provider directory contains a mandatory `manifest.json` file and deserializes `ProviderPackageManifest`. If missing or invalid, discovery aborts.
2. Constructs the provider control pipe path using `ProviderId` from the manifest: `\\.\pipe\<ProviderId>\<UserSID>\control`.
3. Spawns the target provider `.exe` via `Process.Start` with the argument `/pipe`.
4. Connects to the provider's control pipe via `NamedPipeClientStream` with a 5-second timeout.
5. Sends an `info` JSON request message to retrieve provider metadata (`provider_name`, `version`, supported audio formats).
6. Sends a `voices` JSON request message to retrieve the array of available voice models (`id`, `name`, `language`, `gender`, `vendor`).
7. Sends a `shutdown` JSON control message and ensures the background process terminates cleanly.

---

### Provider Installation Engine (`ProviderInstaller.cs`)

`ProviderInstaller` handles package inspection, mandatory `manifest.json` validation, process lock detection, process tree termination, and root-directory-stripped ZIP extraction.

#### Inspection, Lock Detection & Root Stripping

- **Inspection**: Opens the ZIP archive in memory using `ZipArchive` and inspects `manifest.json`.
- **Mandatory Manifest Rule**: Packages MUST contain a valid `manifest.json` with a non-empty `provider_id`. If `manifest.json` is missing or invalid, inspection fails immediately.
- **Process Lock Check**: Queries `Process.GetProcessesByName(executableName)`. If matching processes are running, sets `IsRunningProcessDetected = true` and records `RunningProcessId`.
- **Process Termination**: Offers `TerminateProcess(processId)`, which kills active process trees using `proc.Kill(entireProcessTree: true)` and waits for process exit.
- **Root Directory Stripping Extraction**: Detects if all files in the ZIP archive share a single top-level directory wrapper (e.g., `SampleProvider_v1.0/`). If present, automatically strips the root directory prefix during extraction so files land cleanly inside `.\providers\<ProviderId>\`.

---

## UI Architecture & Accessibility Design

`SapiManager` provides a modern WPF desktop interface styled with WinUI 3 opaque card layouts and backdrop dimming.

### Window Styling & Overlay

- **Main Dashboard (`MainWindow.xaml`)**: Dual-tab interface containing Tab 1 (Provider Management) and Tab 2 (SAPI 5 Speech Tester).
- **Modal Backdrop Overlay**: Uses a full-window `<Border x:Name="ModalOverlay" Background="#66000000"/>` toggled during `ShowDialog()` to dim the main window while modal dialogs (such as `AddProviderWindow`) are open.

### Screen Reader Accessibility

- Keyboard focus is explicitly routed to primary headings on window load (`Focusable="True"`, `KeyboardNavigation.TabIndex="0"`).
- Accessible control labels use `AutomationProperties.Name` directly without appending redundant type strings.

---

## End-to-End Execution Workflows

### Workflow: Package Ingestion & Installation

1. **Package Selection**: User clicks **Browse** on `AddProviderWindow` to select a provider `.zip` file.
2. **Inspection**: `ProviderInstaller.InspectPackage()` inspects package metadata via mandatory `manifest.json` in memory.
3. **Lock Verification**: System checks if the provider executable is currently running. If locked, a warning banner displays the lock status alongside a **Close Running Process** button.
4. **Remediation & Extraction**: User terminates locking processes if necessary, then clicks **Install Provider**. The archive extracts with root directory stripping (e.g. removing `SampleProvider_v1/`) to `.\providers\<ProviderId>\`.
5. **Dashboard Refresh**: `AddProviderWindow` closes with `DialogResult = true`, triggering `ScanProvidersAsync()` to update the provider list.

### Workflow: Provider Probing & Voice Registration

1. **Provider Discovery**: `ScanProvidersAsync()` enumerates subdirectories under `.\providers\`.
2. **IPC Probing**: For each discovered `.exe`, `ProviderDiscovery.ProbeProviderAsync()` checks mandatory `manifest.json` and queries the provider's voices via named pipe IPC.
3. **Voice Management**: Selecting a provider displays its voices in `LstVoices`. Checkboxes indicate whether each voice token is currently registered in HKLM.
4. **Token Persistence**: User checks or unchecks voices and clicks **Save Voices**. `RegistryManager` writes or deletes tokens under `HKLM\SOFTWARE\Microsoft\Speech\Voices\Tokens`.

### Workflow: SAPI 5 Synthesis Verification

1. **Synthesizer Initialization**: Switching to the **SAPI 5 Tester** tab initializes `System.Speech.Synthesis.SpeechSynthesizer`.
2. **Voice Selection & Controls**: Installed SAPI 5 voices (including registered `ModernSapiAdapter` tokens) populate `CmbTestVoice`. User configures Rate, Volume, Pitch, and plain text or SSML.
3. **Speech Synthesis**: User clicks **Speak**. `SpeechSynthesizer` invokes `CoreEngine.dll` via standard SAPI 5 COM interfaces.
4. **Telemetry Logging**: Event callbacks (`SpeakStarted`, `SpeakProgress`, `BookmarkReached`, `SpeakCompleted`) log real-time telemetry to the event log box.
