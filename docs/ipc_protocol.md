# Dual Named Pipe JSON IPC Protocol Specification

The **Modern SAPI Adapter** decouples SAPI 5 COM proxy operations from external Text-To-Speech (TTS) engines using an Inter-Process Communication (IPC) protocol built on Windows Dual Named Pipes and UTF-8 JSON messaging.

This specification details the pipe path security model, control message schemas, raw audio streaming parameters, and asynchronous speech event structures.

---

## Module & Protocol Requirements

| Requirement | Value |
| :--- | :--- |
| **Target Platform** | Windows 11 (x64, ARM64) |
| **Control Pipe Format** | Bi-directional, newline-delimited UTF-8 JSON (`PIPE_READMODE_BYTE`) |
| **Audio Pipe Format** | Inbound, provider-native raw PCM streaming (`PIPE_READMODE_BYTE`) |
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

## Responsiveness and Failure Boundaries

Providers must treat every connected session as latency-sensitive. CoreEngine enforces the following local deadlines so an unresponsive provider cannot hold a SAPI host or screen reader indefinitely:

- Pipe creation after provider launch retains the existing 1,000 ms startup deadline.
- A synchronous Control Pipe write and the initialization `info` response have a 1,500 ms deadline.
- An active `sapi_speak` request has a 1,500 ms inactivity deadline. Matching PCM, word, sentence, bookmark, completion, cancellation, or log activity resets this deadline; total synthesis duration is not capped while progress continues.
- A cancellation transaction has one 500 ms deadline shared by the `cancel` write, `synthesis_cancelled` acknowledgement, and draining exactly `audio_bytes_written` bytes.
- A completed pipe error, disconnect, access denial, malformed terminal boundary, or expired deadline faults the session immediately. CoreEngine cancels outstanding pipe I/O, discards all later output from that session, returns an error to SAPI, and creates a fresh provider session on a later `Speak`.
- Idle audio and control reads may remain pending indefinitely. Deadlines apply only to synchronous operations and active requests, so an idle provider is not disconnected merely for being quiet.

Providers should report matching progress promptly and must complete cancellation cleanup well within 500 ms. These deadlines are CoreEngine recovery policy; they do not add delay to healthy requests.

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
      "text": "Hello ",
      "source_offset": 0,
      "volume": 100,
      "pitch": -5,
      "rate": 2
    },
    {
      "silence_ms": 500
    },
    {
      "bookmark": "section_2"
    },
    {
      "text": "world",
      "source_offset": 17,
      "volume": 100,
      "pitch": 0,
      "rate": 0
    }
  ]
}
```

#### Fragment Properties

| Property | Type | Description |
| :--- | :--- | :--- |
| `text` | String | Plain text string to be synthesized. |
| `source_offset` | Number | Required for text fragments only. A non-negative UTF-16 character offset copied by CoreEngine from `SPVTEXTFRAG::ulTextSrcOffset`. |
| `silence_ms` | Number | Silence duration in milliseconds to insert into the audio stream. |
| `bookmark` | String | Bookmark identifier string emitted as a `bookmark_reached` event when audio reaches this position. |
| `volume` | Number | SAPI volume adjustment (0 to 100). |
| `pitch` | Number | SAPI pitch adjustment (-10 to +10). |
| `rate` | Number | SAPI speaking rate adjustment (-10 to +10). |

---

### `ssml_speak` Command

The source-coordinate guarantees for `sapi_speak` do not apply to `ssml_speak`:
its source document belongs to the direct caller/provider path rather than SAPI
fragment parsing.

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

Instructs the provider to halt active synthesis for a specific request ID. The
provider confirms its completed cleanup with `synthesis_cancelled` before it
accepts a replacement request.

A request remains cancellable until its final PCM byte has been committed to
the audio pipe. This includes the interval after the provider has sent
`synthesis_complete` but while its audio-output writer is still draining.

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

`text_offset` is an absolute UTF-16 offset into the original text passed to
`ISpVoice::Speak`, and `text_length` is a UTF-16 character length. Providers
own native SDK/SSML mapping and CoreEngine forwards these normalized values
unchanged.

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

`text_offset` is an absolute UTF-16 offset into the original text passed to
`ISpVoice::Speak`, and `text_length` is a UTF-16 character length. Providers
own native SDK/SSML mapping and CoreEngine forwards these normalized values
unchanged.

`CoreEngine` maps this payload directly to SAPI event `SPEI_SENTENCE_BOUNDARY`.

#### Unmappable Boundary Coordinates

If a provider cannot confidently map a native word or sentence boundary, it
must suppress only that boundary. It must continue PCM, terminal events, and
later mappable events. Providers must not use guessed fallback coordinates,
including zero. For every suppressed boundary, the provider must send a `log`
event with `severity: "warning"`, the affected `speak_id`, and a message that
contains the native callback offset.

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

### `synthesis_complete` Event

The required completion declaration for a `sapi_speak` request. It means:

> Synthesis for `speak_id` is complete. The complete raw PCM stream produced for
> that request contains exactly `total_audio_bytes` bytes.

```json
{
  "event": "synthesis_complete",
  "speak_id": 1,
  "total_audio_bytes": 62400
}
```

#### Provider requirements

- Send exactly one `synthesis_complete` event for a successfully completed request.
- Preserve the `speak_id` from the corresponding `sapi_speak` command.
- `total_audio_bytes` is a non-negative integer equal to the sum of all raw PCM
  buffers produced for this request, in the `audio_format` returned by `info`.
  It includes silence and must be a multiple of the format's block alignment
  (`channels * bits_per_sample / 8`).
- Count a buffer when it is accepted by the provider's final audio-output queue.
  A provider may send `synthesis_complete` before a background named-pipe writer
  has drained that queue; it must not add latency by waiting for the drain.
- Continue to accept `cancel` for the request until that writer has committed
  the final declared byte. If cancellation arrives in that interval, stop the
  writer and send `synthesis_cancelled`; the cancellation event supersedes
  normal completion for CoreEngine.
- Send all word-boundary and bookmark events for the request before this event on
  the Control Pipe. Do not send normal speech events after it for the same
  `speak_id`.
- Do not send the legacy `completed` event. Cancellation uses
  `synthesis_cancelled`; failures use the `log` event.

#### CoreEngine requirements

`synthesis_complete` declares the final byte boundary; it does not mean that all
audio has arrived at CoreEngine yet. CoreEngine continues forwarding audio to SAPI
immediately and considers the request finished only after it has successfully
passed exactly `total_audio_bytes` to `ISpTTSEngineSite::Write`. A different byte
count is a protocol error.

---

### `synthesis_cancelled` Event

The required terminal event after CoreEngine sends `cancel` for an active
`speak_id`. It confirms that the provider has stopped synthesis, discarded
uncommitted audio for that request, and can accept another `sapi_speak`.

```json
{
  "event": "synthesis_cancelled",
  "speak_id": 1,
  "audio_bytes_written": 18432
}
```

#### Provider requirements

- Send exactly one `synthesis_cancelled` event for a successfully handled
  `cancel` command.
- Preserve the `speak_id` from the cancelled request.
- `audio_bytes_written` is a non-negative integer equal to the number of raw
  PCM bytes from that request that have been committed to the audio named pipe.
  It is not the number of bytes merely accepted into an internal queue, and it
  must be a multiple of the negotiated format's block alignment.
- Before sending the event, discard any uncommitted audio and wait for any
  in-flight audio-pipe write to finish. After the event, write no more PCM or
  normal speech events for that `speak_id`.
- After sending the event, the provider is ready to accept a replacement
  `sapi_speak` command.

#### CoreEngine requirements

CoreEngine does not forward cancelled PCM to SAPI. It drains bytes from the
audio pipe until it has consumed exactly `audio_bytes_written` bytes for the
cancelled request, then may submit the replacement request. A different byte
count is a protocol error.

---

### `log` Event

Fired if the provider encounters an issue, diagnostic event, or synthesis failure.

```json
{
  "event": "log",
  "speak_id": 1,
  "severity": "error",
  "message": "HTTP 401 Unauthorized. Failed to connect to TTS API.",
  "friendly_text": "TTS Provider failed to authenticate. Check your configuration."
}
```

#### Log Severities

| Value | Description |
| :--- | :--- |
| `info` | Informational diagnostic message. Synthesis continues. |
| `warning` | A non-critical issue occurred. Synthesis continues. |
| `error` | Speech-level failure. The current utterance aborts, but the provider process is still healthy and CoreEngine can issue future requests. |
| `fatal` | Provider-level failure. An unrecoverable error occurred (e.g. repeated network disconnects, API auth failure, bad config). The provider process cannot continue, and the connection should be treated as dead. |
```

---

## Audio Pipe Protocol

The Audio Pipe is a unidirectional, raw byte stream (`PIPE_READMODE_BYTE`).

- **Headerless Raw PCM:** Audio data is streamed as uncompressed PCM samples without WAV headers.
- **Frame Alignment:** Byte-mode pipe read boundaries are arbitrary and may split a PCM frame. CoreEngine reconstructs complete frames using the negotiated block alignment (`channels * bits_per_sample / 8`), forwards complete frames immediately, and retains at most one partial frame until the next read.
- **Terminal Alignment:** Both `synthesis_complete.total_audio_bytes` and `synthesis_cancelled.audio_bytes_written` must be multiples of the negotiated block alignment. A trailing partial frame is a provider protocol error; CoreEngine never pads it.
- **Sample Rate:** The sample rate, bit depth, and channel count must match the parameters returned in the `info` response.
