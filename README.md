# Modern SAPI Adapter 2.0

> **Welcome to the Out-Of-Process (OOP) Reboot.**

For two decades, Microsoft SAPI 5 has locked Text-To-Speech  voices inside a rigid, in-process, memory-leaking COM boundary. If a modern voice crashed, the screen reader crashed with it. If there was a memory leak in the voice, it took down the entire host process.

No more.

Modern SAPI Adapter 2.0 completely shatters this limitation. This project aims to rebuild the architecture from the ground up to entirely decouple the legacy SAPI ecosystem from modern AI and neural TTS providers using a lightning-fast, JSON-based named pipe IPC protocol. 

## The Core Architecture
This repository contains exactly two components:
1. **The CoreEngine (C++):** A headless, ultra-low-latency SAPI 5 COM proxy. It does zero audio processing. It simply parses legacy SAPI commands and routes them across a Named Pipe.
2. **The SapiManager (C#):** A modern Windows Desktop application to securely map your SAPI registry keys to external providers.

Providers are no longer built here. Any providers (cloud APIs, local AI engines, etc.) are now completely autonomous, standalone executables written in any language (C#, Python, Rust) that simply expose the IPC pipes. If a provider crashes, the pipe closes, the CoreEngine recovers gracefully, and the host application never skips a beat.

## The `v1` Legacy Vault
If you are looking for the original, in-process DLL implementations, you will find them safely preserved in the `v1/` directory. 

**Note:** The `v1/` directory is strictly **read-only**. It exists purely as a historical reference and inspiration for the native C++ COM interop boilerplate. All active development is happening here in the root.

---
*Ready to build the future of accessible TTS? Read the JSON schema in `docs/ipc_protocol.md` to see the new protocol contract.*
