# SapiSsmlParser.Cpp

A C++20 static library responsible for translating SAPI 5 `ProviderSpeechFragment` sequences into formatted W3C SSML documents for speech synthesis providers.

## Project Specifications
- Language Standard: C++20
- Target Architectures: ModernSapiAdapter ecosystem

## Architecture & Responsibilities

### Data Contract & Output Structure
The parser exposes `SapiSsmlParser::Parse(...)` which accepts:
- `const ProviderSpeechFragment* fragments` (array of unmanaged ABI fragments)
- `uint32_t count` (number of fragments)
- `const std::wstring& localeName` (voice/locale tag, e.g. `en-US`, `ja-JP`)

And returns a `SsmlParseResult` containing:
- `std::u16string SsmlString`: The generated UTF-16 SSML document.
- `std::vector<std::pair<uint32_t, uint32_t>> OffsetMap`: Mapping of SSML character indices back to original SAPI text offsets (`OriginalOffset`).

### SSML Structure & Escaping
- Root element wrapper: `<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xmlns:mstts='http://www.w3.org/2001/mstts' xml:lang='[localeName]'>`
- All input text is escaped for XML entities (`<`, `>`, `&`, `"`, `'`).

### Bookmarks
Fragments with `Action == PROVIDER_ACTION_BOOKMARK` are emitted as SSML bookmark elements:
`<bookmark mark='[Escaped Fragment Text]'/>`

### Prosody Formula Remapping
Speakable fragments (`PROVIDER_ACTION_SPEAK`, `SPELL_OUT`, `PRONOUNCE`) evaluate pitch, volume, and rate adjustments relative to default values (`Volume = 100`, `Pitch = 0`, `Rate = 0`). Non-default values are wrapped in `<prosody>`:
- Pitch: `Pitch 5.0f` (maps `-10..+10` to `-50%..+50%`)
- Volume: `Volume - 100.0f` (maps `0..100` to `-100%..0%`)
- Rate: `Rate >= 0 ? Rate * 20.0f : Rate * 20.0f / 3.0f` (maps `-10..+10` to `-66%..+200%`)

### NeedAddingSpace Algorithm
To prevent merging of adjacent words across fragment boundaries while preserving natural Asian TTS phrasing, spaces are injected conditionally:
- Rule: Injects space if previous text ends with non-space character and new text starts with non-space character.
- CJK Exception: Space injection is bypassed when either adjacent character is a CJK character (evaluated via Win32 `GetStringTypeW` for `C3_IDEOGRAPH | C3_KATAKANA | C3_HIRAGANA`).
