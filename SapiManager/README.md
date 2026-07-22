# SapiManager

A modern C# WPF application targeting **.NET 8** that serves as the central administrative management suite, installer, and SAPI 5 registry broker for the `ModernSapiAdapter` ecosystem.

---

## Architectural Overview & Responsibilities

`SapiManager` is designed as a portable, self-installing utility application that bridges speech provider modules with the Windows SAPI 5 subsystem and COM registration framework.

### Key Responsibilities

1. **Startup Routing & Portable Self-Installation (`App.xaml.cs` & `Views/InstallerWindow.xaml`)**
   - Inspects the launch arguments and execution directory upon startup.
   - **Install Mode:** If launched from outside `C:\Program Files\ModernSapiAdapter` without arguments, it displays the setup wizard, copies the binaries to `Program Files`, registers `CoreEngine.dll` with COM, creates a Start Menu shortcut (`ShortcutManager.cs`), writes Windows Add/Remove Programs registry entries, and launches the installed application.
   - **Uninstall Mode:** If launched with the `/uninstall` command-line argument, it opens the uninstallation view, unregisters `CoreEngine.dll` from COM, removes the Start Menu shortcut, cleans up all `ModernSapiAdapter` voice tokens from the registry, and deletes installation files.
   - **Dashboard Mode:** If running inside `C:\Program Files\ModernSapiAdapter` without arguments, it launches the main management workspace.

2. **SAPI 5 Voice Token Management (`Services/RegistryManager.cs`)**
   - Writes voice tokens to the 64-bit registry hive: `HKLM\SOFTWARE\Microsoft\Speech\Voices\Tokens`.
   - Automatically converts BCP-47 language tags (e.g. `en-US`) to SAPI 5 hex LCIDs (e.g. `409`).
   - Links voice tokens directly to `CoreEngine.dll` CLSID `{B7E2E0A6-A067-4286-9A38-9FE7FA25C98D}` and the provider DLL path.

3. **CoreEngine COM Registration (`Services/ComRegistrar.cs`)**
   - Dynamically loads `CoreEngine.dll` in-process using P/Invoke (`LoadLibraryEx`) and invokes `DllRegisterServer` / `DllUnregisterServer` entry points.

4. **Start Menu Shortcut Broker (`Services/ShortcutManager.cs`)**
   - Creates and removes `ModernSapiAdapter Speech Manager.lnk` in `C:\ProgramData\Microsoft\Windows\Start Menu\Programs` via native COM interop (`IShellLinkW` / `IPersistFile`).

5. **Provider Discovery & Dynamic Schema GUI (`MainWindow.xaml`)**
   - Scans the `providers/` directory adjacent to the application executable.
   - Reads `<ProviderName>_voices.json` manifests to discover available voices and dynamic configuration schemas (`configSchema`).
   - Dynamically instantiates WPF controls (`TextBox`, `CheckBox`) in code-behind based on schema definitions without requiring hardcoded UI components for individual providers.

6. **Dual-Tier Configuration & Credential Security (`Services/CredentialManager.cs`)**
   - **Configuration Hierarchy Model:**
     - **Level 1 — Machine-Wide Baseline:** `<ModuleDir>\<ProviderName>_config.json` inside the provider folder.
     - **Level 2 — User-Wide Override (Highest Priority):** `%LOCALAPPDATA%\ModernSapiAdapter\Config\<ProviderName>_config.json`.
     - Scalar settings (e.g. `DecryptionKey`) in User-Wide config override Machine-Wide settings; path arrays (e.g. `ExtraVoicePaths`) are merged/deduplicated.
   - **Primary Tier (User-Specific):** Securely stores API keys in Windows Credential Manager via P/Invoke (`CredReadW` / `CredWriteW`).
   - **Fallback Tier (Machine-Wide):** Encrypts credentials using Windows DPAPI (`DataProtectionScope.LocalMachine`) and writes encrypted Base64 blobs to `<ProviderName>_config.json` for system-level/logon screen access.

7. **Screen Reader Accessibility (JAWS & NVDA Optimization)**
   - Fully optimized for screen reader accessibility:
   - All controls, list items, tabs, and dynamic schema inputs specify explicit `AutomationProperties.Name` and `AutomationProperties.HelpText`.
   - Initial focus management in dialogs ensures screen readers immediately speak window context upon launch.

8. **SAPI 5 Voice Tester Workspace (`MainWindow.xaml` & `System.Speech.Synthesis`)**
   - Enables real-time testing of any SAPI 5 voice registered in Windows or belonging to the selected provider.
   - Provides interactive Speech Rate, Volume, and Pitch sliders.
   - Supports plain text and W3C/SAPI SSML XML synthesis mode with sample text presets.
   - Captures and displays real-time event telemetry (`SpeakStarted`, `SpeakProgress`, `BookmarkReached`, `SpeakCompleted`) with character offset tracking and bookmark logging.

---

## Build & Development Guidelines

### Target Requirements
- **Framework:** `.NET 8.0-windows`
- **Platform:** `x64` / `ARM64` (32-bit `Win32` targets unsupported)
- **Manifest:** Embedded `app.manifest` requiring `requireAdministrator`.

### Compilation
To compile `SapiManager` independently via the command line or MSBuild:

```cmd
dotnet build SapiManager/SapiManager.csproj -p:Platform=x64 -c Release
```

Output binaries are centrally generated under `bin/SapiManager/x64/Release/` as configured by `Directory.Build.props`.

---

## Project Structure

```
SapiManager/
├── app.manifest                 # Mandates requireAdministrator for system registry & Program Files writes
├── App.xaml / App.xaml.cs       # Application entry point & startup router
├── MainWindow.xaml / .cs        # Main dashboard & dynamic schema UI generator
├── SapiManager.csproj           # .NET 8 WPF project file (Single-file publish enabled)
├── Models/
│   └── ProviderManifest.cs      # Data models for provider manifests & user config JSONs
├── Services/
│   ├── ComRegistrar.cs          # P/Invoke wrapper for CoreEngine.dll COM registration
│   ├── CredentialManager.cs     # Windows Credential Manager & DPAPI encryption service
│   ├── RegistryManager.cs       # SAPI 5 voice token & uninstall registry manager
│   └── ShortcutManager.cs       # Native COM interop Start Menu shortcut (.lnk) manager
└── Views/
    └── InstallerWindow.xaml / .cs # Setup & Uninstall wizard window
```
