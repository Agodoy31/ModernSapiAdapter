# Dual Named Pipe JSON IPC Protocol Specification

The **Modern SAPI Adapter** decouples SAPI 5 COM proxy operations from external Text-To-Speech (TTS) engines using an Inter-Process Communication (IPC) protocol built on Windows Dual Named Pipes and UTF-8 JSON messaging.

This specification details the pipe path security model, control message schemas, raw audio streaming parameters, and asynchronous speech event structures.

---

## Module & Protocol Requirements

| Requirement | Value |
| :--- | :--- |
| **Target Platform** | Windows 11 (x64, ARM64) |
| **Control Pipe Format** | Bi-directional, newline-delimited UTF-8 JSON (`PIPE_READMODE_MESSAGE`) |
| **Audio Pipe Format** | Inbound, raw 16-bit Mono PCM streaming (`PIPE_READMODE_BYTE`) |
| **Security Isolation** | Dynamic Windows User SID path injection |
| **Default Audio Format** | 24,000 Hz, 16-bit Mono PCM |

---

## Pipe Paths & Security Isolation

The proxy (`CoreEngine`) and provider executables communicate using two dedicated named pipe channels constructed with the user's security identifier (`UserSID`):

- **Control Pipe Path:** `\\.\pipe\<ProviderPipeName>\<UserSID>\control`
- **Audio Pipe Path:** `\\.\pipe\<ProviderPipeName>\<UserSID>\audio`

### Security Rationale

`UserSID` is dynamically resolved at runtime (e.g., `S-1-5-21-...`) on both sides of the pipe connection:
1. **Fast User Switching:** Prevents cross-session pipe collisions when multiple Windows user accounts are logged in simultaneously.
2. **UAC & Secure Desktop Isolation:** Ensures elevated admin or system processes cannot hijack or read unprivileged user speech streams.

> [!NOTE]
> The provider process creates the named pipe servers upon startup. If `CoreEngine` attempts to connect before the provider is running, `CoreEngine` spawns the provider executable via `CreateProcessW` and retries the connection.

---

## Control Pipe Messages (Proxy -> Provider)

Control pipe messages are sent as single-line, newline-terminated (`\n`) UTF-8 JSON objects.

### `info` Command

Queries the provider's supported audio output format and capabilities during initialization.

#### Request (Proxy -> Provider)
```json
{
  "command": "info"
}
```

#### Response (Provider -> Proxy)
```json
{
  "response": "info",
  "provider_name": "MyTTS Provider",
  "version": "1.0.0",
  "supports_ssml": true,
  "audio_format": {
    "sample_rate": 24000,
    "bits_per_sample": 16,
    "channels": 1
  }
}
```

---

### `voices` Command

Used by `SapiManager` to query the list of available voice models from a provider executable.

#### Request (Client -> Provider)
```json
{
  "command": "voices"
}
```

#### Response (Provider -> Client)
```json
{
  "response": "voices",
  "voices": [
    {
      "id": "voice_en_us_1",
      "name": "MyTTS Female Voice",
      "language": "en-US",
      "gender": "female",
      "vendor": "MyTTS Inc"
    },
    {
      "id": "voice_en_us_2",
      "name": "MyTTS Male Voice",
      "language": "en-US",
      "gender": "male",
      "vendor": "MyTTS Inc"
    }
  ]
}
```

#### Voice Properties

| Property | Type | Description |
| :--- | :--- | :--- |
| `id` | String | Unique identifier for the voice model. |
| `name` | String | Display name of the voice. |
| `language` | String | BCP-47 language tag (e.g., `en-US`, `es-ES`). |
| `gender` | String | The gender of the voice. Valid values: `male`, `female`, `neutral`. |
| `vendor` | String | The manufacturer or vendor of the voice engine. |
```

---

### `sapi_speak` Command

Requests speech synthesis for a collection of SAPI text, silence, or bookmark fragments.

#### Request (Proxy -> Provider)
```json
{
  "command": "sapi_speak",
  "speak_id": 1,
  "voice_id": "voice_en_us_1",
  "fragments": [
    {
      "text": "Hello world.",
      "volume": 100,
      "pitch": -5,
      "rate": 2
    },
    {
      "silence_ms": 500
    },
    {
      "bookmark": "section_2"
    }
  ]
}
```

#### Fragment Properties

| Property | Type | Description |
| :--- | :--- | :--- |
| `text` | String | Plain text string to be synthesized. |
| `silence_ms` | Number | Silence duration in milliseconds to insert into the audio stream. |
| `bookmark` | String | Bookmark identifier string emitted as a `bookmark_reached` event when audio reaches this position. |
| `volume` | Number | SAPI volume adjustment (0 to 100). |
| `pitch` | Number | SAPI pitch adjustment (-10 to +10). |
| `rate` | Number | SAPI speaking rate adjustment (-10 to +10). |

---

### `ssml_speak` Command

Allows direct SSML synthesis requests from modern clients, bypassing legacy SAPI 5 fragment conversion.

#### Request (Client -> Provider)
```json
{
  "command": "ssml_speak",
  "speak_id": 2,
  "voice_id": "voice_en_us_1",
  "ssml": "<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='en-US'><prosody pitch='+5%'>Hello world</prosody></speak>"
}
```

---

### `cancel` Command

Instructs the provider to immediately halt active speech synthesis for a specific request ID and purge queued audio.

#### Request (Proxy -> Provider)
```json
{
  "command": "cancel",
  "speak_id": 1
}
```

---

### `shutdown` Command

Instructs the provider process to gracefully terminate itself. This is typically sent by management applications like `SapiManager` after probing a provider for its capabilities, allowing the provider to clean up resources and exit cleanly.

#### Request (Client -> Provider)
```json
{
  "command": "shutdown"
}
```

---

## Asynchronous Events (Provider -> Proxy)

While generating raw PCM audio on the Audio Pipe, the provider concurrently sends JSON event objects across the Control Pipe to keep the screen reader synchronized.

### `word_boundary` Event

Fired when audio synthesis reaches a word boundary in the source text.

```json
{
  "event": "word_boundary",
  "speak_id": 1,
  "text_offset": 0,
  "text_length": 5,
  "audio_offset_ms": 150
}
```

`CoreEngine` maps this payload directly to SAPI event `SPEI_WORD_BOUNDARY`.

---

### `sentence_boundary` Event

Fired when audio synthesis reaches a sentence boundary.

```json
{
  "event": "sentence_boundary",
  "speak_id": 1,
  "text_offset": 0,
  "text_length": 25,
  "audio_offset_ms": 150
}
```

`CoreEngine` maps this payload directly to SAPI event `SPEI_SENTENCE_BOUNDARY`.

---

### `bookmark_reached` Event

Fired when audio synthesis reaches a requested bookmark fragment.

```json
{
  "event": "bookmark_reached",
  "speak_id": 1,
  "bookmark_name": "section_2",
  "audio_offset_ms": 1250
}
```

`CoreEngine` maps this payload directly to SAPI event `SPEI_TTS_BOOKMARK`.

---

### `completed` Event

Fired by the provider when all audio fragments for a speech request have been written to the Audio Pipe.

```json
{
  "event": "completed",
  "speak_id": 1
}
```

Signals `CoreEngine` worker threads that speech synthesis for `speak_id` is finished.

---

### `error` Event

Fired if the provider encounters a synthesis failure.

```json
{
  "event": "error",
  "speak_id": 1,
  "severity": "fatal",
  "message": "HTTP 401 Unauthorized. Failed to connect to TTS API.",
  "friendly_text": "TTS Provider failed to authenticate. Check your configuration."
}
```

#### Error Severities

| Value | Description |
| :--- | :--- |
| `info` | Informational diagnostic message. Synthesis continues. |
| `warning` | A non-critical issue occurred. Synthesis continues. |
| `error` | A specific fragment or request failed, but the provider process remains stable. |
| `fatal` | An unrecoverable error occurred (e.g. network disconnect, API auth failure). Synthesis aborts. |
```

---

## Audio Pipe Protocol

The Audio Pipe is a unidirectional, raw byte stream (`PIPE_READMODE_BYTE`).

- **Headerless Raw PCM:** Audio data is streamed as uncompressed PCM samples without WAV headers.
- **Buffer Alignment:** Audio chunks are read in 4096-byte buffers and passed directly to `ISpTTSEngineSite::Write()`.
- **Sample Rate:** The sample rate, bit depth, and channel count must match the parameters returned in the `info` response.
