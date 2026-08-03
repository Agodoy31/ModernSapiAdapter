# Modern SAPI Adapter - IPC JSON Specification

This document outlines the standard JSON Inter-Process Communication (IPC) protocol used between the C++ SAPI Proxy (`CoreEngine`) and standalone TTS Providers.

## Pipe Naming Convention
Pipes are created dynamically to ensure Fast User Switching and Secure Desktop (UAC) isolation.
- **Control Pipe:** `\\.\pipe\[provider_id]\[UserSID]\control`
- **Audio Pipe:** `\\.\pipe\[provider_id]\[UserSID]\audio`

*(Example: `\\.\pipe\azure_tts_provider\S-1-5-21-123456789\control`)*

---

## 1. Initialization Commands

### Get Info
The Proxy asks the Provider for its capabilities and audio format.

**Proxy -> Provider (info request)**
```json
{
  "command": "info"
}
```

**Provider -> Proxy (info response)**
```json
{
  "response": "info",
  "provider_name": "Modern SAPI Azure Provider",
  "version": "1.0.0",
  "supports_ssml": true,
  "audio_format": {
    "sample_rate": 24000,
    "bits_per_sample": 16,
    "channels": 1
  }
}
```

### Get Voices
The Proxy requests the list of installed voices from the Provider.

**Proxy -> Provider (voices request)**
```json
{
  "command": "voices"
}
```

**Provider -> Proxy (voices response)**
```json
{
  "response": "voices",
  "voices": [
    { 
      "id": "jenny_v1", 
      "name": "Microsoft Jenny", 
      "language": "en-US", 
      "gender": "female",
      "vendor": "Microsoft"
    },
    { 
      "id": "guy_v1", 
      "name": "Microsoft Guy", 
      "language": "en-US", 
      "gender": "male",
      "vendor": "Microsoft"
    }
  ]
}
```

---

## 2. Speech Synthesis

### SAPI Speak (Legacy SAPI 5 compatibility)
The Proxy strips or parses SAPI XML into fragments and passes the *raw* SAPI tuning values (integers) directly to the Provider.

**Proxy -> Provider (sapi_speak)**
```json
{
  "command": "sapi_speak",
  "speak_id": 1,
  "voice_id": "jenny_v1",
  "fragments": [
    { "text": "Hello world.", "volume": 100, "pitch": -5, "rate": 2 },
    { "silence_ms": 500 },
    { "bookmark": "section_2" }
  ]
}
```

### SSML Speak (Modern Clients)
Modern external clients can optionally connect directly to the pipe and bypass SAPI tuning entirely.

**Client -> Provider (ssml_speak)**
```json
{
  "command": "ssml_speak",
  "speak_id": 2,
  "voice_id": "jenny_v1",
  "ssml": "<speak><prosody pitch='+5%'>Hello</prosody></speak>"
}
```

### Cancel
Flushes the synthesis pipeline instantly.

**Proxy -> Provider (cancel)**
```json
{
  "command": "cancel",
  "speak_id": 1
}
```

---

## 3. Asynchronous Events
As the Provider generates raw PCM audio on the Audio Pipe, it concurrently fires JSON events back down the Control Pipe to keep the screen reader synchronized.

### Word Boundary
**Provider -> Proxy**
```json
{
  "event": "word_boundary",
  "speak_id": 1,
  "text_offset": 0,
  "text_length": 5,
  "audio_offset_ms": 150
}
```

### Bookmark Reached
**Provider -> Proxy**
```json
{
  "event": "bookmark_reached",
  "speak_id": 1,
  "bookmark_name": "section_2",
  "audio_offset_ms": 1250
}
```

---

## 4. Error Handling
If the Provider encounters a failure, it informs the proxy gracefully instead of crashing silently.

**Provider -> Proxy (error event)**
```json
{
  "event": "error",
  "speak_id": 1,
  "severity": "fatal",
  "message": "HTTP 401 Unauthorized. Failed to connect to Azure API.",
  "friendly_text": "Azure TTS failed to authenticate. Check your API key."
}
```
