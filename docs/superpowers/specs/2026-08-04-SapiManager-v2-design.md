# Design Spec: SapiManager 2.0 (Out-Of-Process Reboot)

## Purpose & Scope
The `SapiManager` is shifting from its legacy role (DPAPI credential vault and config form generator) to a pure **SAPI 5 Registry Harness and IPC Orchestrator**. Its job is to discover external standalone Provider executables, extract their voice capabilities via a Named Pipe JSON IPC, and register those voices into the Windows SAPI ecosystem (`HKLM`) pointing to the unmanaged `CoreEngine.dll` proxy.

## 1. Discovery & IPC Orchestration
- **The Probe Lifecycle:** 
  - `SapiManager` discovers provider executables via a hybrid approach: auto-scanning a `Providers/` folder and allowing manual "Add Provider" file browsing.
  - Upon discovery, `SapiManager` executes `Provider.exe /pipe`, which outputs a dynamically generated `provider_id` to standard output.
- **The Handshake:** 
  - `SapiManager` establishes a Named Pipe connection to `\\.\pipe\[provider_id]\[UserSID]\control`.
  - It sends the `info` and `voices` JSON commands to receive the provider's capabilities.
  - *Note: We will add a `shutdown` command to the JSON Schema so `SapiManager` can cleanly instruct the Provider process to exit after probing.*

## 2. UI Architecture
- **UAC Admin Requirement:** The `app.manifest` will strictly enforce `requireAdministrator` to ensure it has permissions to write to `HKLM`.
- **The Orchestrator Dashboard:** 
  - **Left Sidebar:** A master list of all discovered external Providers.
  - **Main Dashboard (Right Pane):**
    - A massive, prominent "Configure Provider" button. Clicking this executes `Provider.exe /config`, entirely delegating UI and API key storage to the external process.
    - A rich DataGrid below it displaying the voices exposed by that provider. Checkboxes allow users to register or unregister individual voices.
- **The SAPI 5 Voice Tester:** 
  - The existing SAPI 5 testing module from v1 will be retained and refactored for v2 (a dedicated tab), providing a robust baseline to test synthesized audio and verify telemetry.

## 3. SAPI 5 Registry Mapping
- When a user enables a voice checkbox, `SapiManager` writes the SAPI 5 token to `HKLM\SOFTWARE\Microsoft\Speech\Voices\Tokens`.
- The token sets its `CLSID` to the headless `CoreEngine.dll` COM proxy.
- `SapiManager` injects critical string attributes into the token (e.g., `ModernSapi_ProviderExePath` and `ModernSapi_ProviderId`) so that `CoreEngine.dll` knows exactly which external `Provider.exe` to launch when the screen reader attempts to speak.

## 4. Testing & Verification
- **Unit Tests / Manual:** Manual validation using the built-in SAPI 5 tester to ensure `SapiManager` correctly discovers the voices, delegates configuration, and seamlessly hands off execution to `CoreEngine.dll`.
