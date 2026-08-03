# Modern SAPI Adapter 2.0

> **Welcome to the Out-Of-Process (OOP) Reboot.**

For two decades, Microsoft SAPI 5 has locked Text-To-Speech (TTS) voices inside a rigid, in-process, memory-leaking COM boundary. If a modern cloud API crashed, the screen reader crashed with it. If there was a memory leak in the provider, it took down the entire host process.

No more.

Modern SAPI Adapter 2.0 completely shatters this limitation. We have rebuilt the architecture from the ground up to entirely decouple the legacy SAPI ecosystem from modern AI TTS providers using a lightning-fast, JSON-based **Named Pipe IPC Protocol**. 

## The Core Architecture
This repository contains exactly two components:
1. **The CoreEngine (C++):** A headless, ultra-low-latency SAPI 5 COM proxy. It does zero audio processing. It simply parses legacy SAPI commands and routes them across a Named Pipe.
2. **The SapiManager (C#):** A modern Windows Desktop application to securely map your SAPI registry keys to external providers.

**Providers are no longer built here.** Providers (Azure, Google, Piper, VITS) are now completely autonomous, standalone executables written in any language (C#, Python, Rust) that simply connect to our IPC pipes. If a provider crashes, the pipe closes, the CoreEngine recovers gracefully, and your screen reader never skips a beat.

## The `v1` Legacy Vault
If you are looking for the original, in-process DLL implementations, you will find them safely preserved in the `v1/` directory. 

**Note:** The `v1/` directory is strictly **read-only**. It exists purely as a historical reference and inspiration for the native C++ COM interop boilerplate. All active development is happening here in the root.

---
*Ready to build the future of accessible TTS? Read the JSON schema in `json_schema.md` to see the new protocol contract.*
