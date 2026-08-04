# SapiManager v2.0 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Overhaul SapiManager to dynamically probe standalone Provider executables via JSON IPC and register voice tokens in HKLM pointing to CoreEngine.dll.

**Architecture:** A WPF desktop app (.NET 8) that requires Admin privileges to write to HKLM. It discovers `Provider.exe` files, executes them with `/pipe` to get a pipe ID, connects to `\\.\pipe\[id]\[UserSID]\control` to fetch JSON capabilities (info, voices), and then sends a `shutdown` command. It registers the SAPI tokens with attributes `ProviderExecutablePath`, `ProviderPipeName`, and `VoiceId`.

**Tech Stack:** C# .NET 8, WPF, System.IO.Pipes, System.Text.Json.

## Global Constraints

- Target Environment: Windows 11 exclusively, strict 64-bit (`x64` / `ARM64`).
- Must ask permission before running MSBuild for a specific task.
- Command line tools (cat/powershell) to modify files are strictly banned; use native tools.
- Do not execute Git operations without explicit permission if you're on the main branch. If you're on a development branch executing git commands autonomusly is fine.

---

### Task 1: Clean Up Legacy V1 Code & Admin Manifest

**Files:**
- Modify: `d:\Projects\ModernSapiAdapter\SapiManager\app.manifest`
- Delete/Modify: Legacy `Models`, `Services`, `Views` and old `MainWindow.xaml` code.

- [ ] **Step 1: Ensure app.manifest requests Administrator privileges**
  Ensure `<requestedExecutionLevel level="requireAdministrator" uiAccess="false" />` is set in `app.manifest`.
- [ ] **Step 2: Clean up legacy provider code**
  Remove the DPAPI `CredentialManager.cs` and old dynamic UI generation logic in `MainWindow.xaml.cs`.
  - [ ] **Step 3: Make sure that for the .net libraries that we'll be using there are no extra nu-get dependencies that need to be installed. If there are then pause and inform before proceeding as the user has to do it manually.
- [ ] **Step 4: Commit**

### Task 2: Implement Provider Probing & IPC Models

**Files:**
- Create: `d:\Projects\ModernSapiAdapter\SapiManager\Services\ProviderProber.cs`
- Create: `d:\Projects\ModernSapiAdapter\SapiManager\Models\IpcModels.cs`

- [ ] **Step 1: Define JSON IPC Models**
  Create classes for `info` and `voices` JSON responses, and a `shutdown` command.
- [ ] **Step 2: Implement `ProviderProber.cs`**
  Write logic to launch `Provider.exe /pipe`, capture standard output for `provider_id`, connect to `\\.\pipe\[provider_id]\[UserSID]\control` using `NamedPipeClientStream`, send `{"command":"info"}`, send `{"command":"voices"}`, and finally send `{"command":"shutdown"}`.
- [ ] **Step 3: Test Probing**
  (Create a mock test using MockProvider and verify compilation via MSBuild).
- [ ] **Step 4: Commit**

### Task 3: Implement SAPI 5 Registry Manager (HKLM)

**Files:**
- Modify: `d:\Projects\ModernSapiAdapter\SapiManager\Services\RegistryManager.cs`

- [ ] **Step 1: Write HKLM Registry Logic**
  Implement methods to write to `HKLM\SOFTWARE\Microsoft\Speech\Voices\Tokens`.
- [ ] **Step 2: Set CoreEngine CLSID & Attributes**
  The SAPI token must set its default value to the voice name, `CLSID` to `{B7E2E0A6-A067-4286-9A38-9FE7FA25C98D}`, and explicitly add string attributes: `ProviderExecutablePath`, `ProviderPipeName`, and `VoiceId`.
- [ ] **Step 3: Commit**

### Task 4: Orchestrator Dashboard UI (Accessibility & Controls Refinement)

**Files:**
- Modify: `d:\Projects\ModernSapiAdapter\SapiManager\MainWindow.xaml`
- Modify: `d:\Projects\ModernSapiAdapter\SapiManager\MainWindow.xaml.cs`

- [ ] **Step 1: Build the Provider List (Left Sidebar)**
  Use a `ListBox` (`LstProviders`) for Discovered Providers. Ensure `AutomationProperties.Name` is cleanly bound to the Provider Name (e.g., "Microsoft Azure TTS") to avoid screen reader verbosity.
- [ ] **Step 2: Build the Provider Settings Panel (Right Pane)**
  This panel is disabled/hidden if no provider is selected.
  Include a `Button` (`BtnConfigure`) that dynamically binds its `AutomationProperties.Name` (e.g., "Configure Microsoft Azure TTS"). Clicking it executes `Process.Start("Provider.exe", "/config")`.
  Include a "Save Voices" `Button` (`BtnSaveVoices`) specifically inside this pane.
- [ ] **Step 3: Build the Voices List (Right Pane)**
  Instead of a verbose DataGrid, use a `ListBox` (`LstVoices`) with a custom `DataTemplate` for the visual layout (showing checkboxes, gender, etc). Crucially, override `AutomationProperties.Name` on the `ListBoxItem` container to read cohesively as one sentence (e.g., "Microsoft Jenny, English US, Female. Unchecked").
- [ ] **Step 4: Wire up the Save logic**
  Checking/unchecking voices in `LstVoices` only updates local state. Clicking `BtnSaveVoices` batches the changes and calls `RegistryManager` to register/unregister the SAPI tokens.
- [ ] **Step 5: Verify UI Builds**
  Run MSBuild to verify there are no XAML or C# errors.
- [ ] **Step 6: Commit**

### Task 5: SAPI 5 Tester Integration

**Files:**
- Modify: `d:\Projects\ModernSapiAdapter\SapiManager\MainWindow.xaml`

- [ ] **Step 1: Retain Tester Module**
  Ensure the existing SAPI 5 tester logic is encapsulated in a dedicated tab or pane alongside the new Dashboard layout.
- [ ] **Step 2: Commit**
