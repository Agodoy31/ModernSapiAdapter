#include "pch.h"
#include "SapiSsmlParser.h"
#include <cmath>
#include <string>

// Helper to determine if a space is needed between two characters
static bool NeedAddingSpace(const std::u16string& left, const std::u16string& right) {
    if (left.empty() || right.empty()) {
        return false;
    }

    wchar_t l = static_cast<wchar_t>(left[0]);
    wchar_t r = static_cast<wchar_t>(right[0]);

    if (l == 0 || r == 0) {
        return false;
    }

    // 1. Whitespace and control characters
    if (iswspace(l) || iswspace(r) || iswcntrl(l) || iswcntrl(r)) {
        return false;
    }

    // 2. Trailing punctuation (no space before these characters)
    if (r == L'.' || r == L',' || r == L'!' || r == L'?' || r == L':' || r == L';' ||
        r == L')' || r == L']' || r == L'}' || r == L'%' || r == L'"' || r == L'\'' ||
        r == L'\u201D' || r == L'\u2019' || r == L'\u00BB') {
        return false;
    }

    // 3. Opening punctuation (no space after these characters)
    if (l == L'(' || l == L'[' || l == L'{' || l == L'<' || l == L'"' || l == L'\'' ||
        l == L'\u201C' || l == L'\u2018' || l == L'\u00AB') {
        return false;
    }

    // 4. Connecting punctuation / symbols (hyphens, dashes, slashes)
    if (l == L'-' || l == L'/' || l == L'\\' || l == L'@' || l == L'\u2013' || l == L'\u2014' ||
        r == L'-' || r == L'/' || r == L'\\' || r == L'@' || r == L'\u2013' || r == L'\u2014') {
        return false;
    }

    // 5. Check if either character is CJK (Ideograph, Katakana, Hiragana)
    WORD typesLeft[2] = {0, 0};
    WORD typesRight[2] = {0, 0};
    
    GetStringTypeW(CT_CTYPE3, reinterpret_cast<LPCWSTR>(left.data()), (int)left.length(), typesLeft);
    GetStringTypeW(CT_CTYPE3, reinterpret_cast<LPCWSTR>(right.data()), (int)right.length(), typesRight);

    for (size_t i = 0; i < left.length(); i++) {
        if (typesLeft[i] & (C3_IDEOGRAPH | C3_KATAKANA | C3_HIRAGANA)) return false;
    }
    for (size_t i = 0; i < right.length(); i++) {
        if (typesRight[i] & (C3_IDEOGRAPH | C3_KATAKANA | C3_HIRAGANA)) return false;
    }

    return true;
}

// Helper to escape XML characters
static void AppendEscapedXml(std::u16string& result, const char16_t* text, uint32_t length) {
    for (uint32_t i = 0; i < length; ++i) {
        char16_t c = text[i];
        if (c == 0) continue; // Ignore embedded nulls to prevent SSML truncation
        switch (c) {
            case u'<': result += u"&lt;"; break;
            case u'>': result += u"&gt;"; break;
            case u'&': result += u"&amp;"; break;
            case u'\"': result += u"&quot;"; break;
            case u'\'': result += u"&apos;"; break;
            default: result += c; break;
        }
    }
}

// Helper to format float into relative percentage e.g. "+50%", "-10%"
static std::u16string FormatProsodyValue(float value) {
    int v = static_cast<int>(std::round(value));
    std::u16string res;
    if (v >= 0) {
        res += u'+';
    }
    std::string s = std::to_string(v);
    for (char c : s) {
        res += static_cast<char16_t>(c);
    }
    res += u'%';
    return res;
}

SsmlParseResult SapiSsmlParser::Parse(const ProviderSpeechFragment* fragments, uint32_t count, const std::wstring& localeName) {
    SsmlParseResult result;
    result.SsmlString.reserve(1024); // Start with a reasonable buffer
    
    // Convert localeName to char16_t string
    std::u16string u16Locale(localeName.begin(), localeName.end());
    
    // Add the SSML root tag
    result.SsmlString += u"<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xmlns:mstts='http://www.w3.org/2001/mstts' xml:lang='";
    result.SsmlString += u16Locale;
    result.SsmlString += u"'>";

    std::u16string lastGrapheme;

    bool isProsodyOpen = false;
    float currentPitch = 0.0f;
    float currentVolume = 100.0f;
    float currentRate = 0.0f;

    for (uint32_t i = 0; i < count; ++i) {
        const auto& frag = fragments[i];

        if (frag.Action == PROVIDER_ACTION_BOOKMARK) {
            result.SsmlString += u"<bookmark mark='OFFSET_";
            
            std::string offsetStr = std::to_string(frag.OriginalOffset);
            std::u16string u16Offset(offsetStr.begin(), offsetStr.end());
            result.SsmlString += u16Offset;
            result.SsmlString += u"_";

            // Still map the offset in OffsetMap just in case, though it's no longer strictly needed for bookmarks
            result.OffsetMap.push_back({ static_cast<uint32_t>(result.SsmlString.length()), frag.OriginalOffset });
            
            AppendEscapedXml(result.SsmlString, frag.Text, frag.TextLength);
            result.SsmlString += u"'/>";
        } else if (frag.Action == PROVIDER_ACTION_SPEAK || 
                   frag.Action == PROVIDER_ACTION_SPELL_OUT || 
                   frag.Action == PROVIDER_ACTION_PRONOUNCE) {
            
            // Extract the first grapheme of the incoming fragment
            std::u16string nextGrapheme;
            if (frag.TextLength >= 2 && frag.Text[0] >= 0xD800 && frag.Text[0] <= 0xDBFF && frag.Text[1] >= 0xDC00 && frag.Text[1] <= 0xDFFF) {
                nextGrapheme.assign(frag.Text, 2);
            } else if (frag.TextLength >= 1) {
                nextGrapheme.assign(frag.Text, 1);
            }

            bool hasProsody = (frag.Pitch != 0.0f) || (frag.Volume != 100.0f) || (frag.Rate != 0.0f);
            
            if (isProsodyOpen) {
                if (!hasProsody || frag.Pitch != currentPitch || frag.Volume != currentVolume || frag.Rate != currentRate) {
                    result.SsmlString += u"</prosody>";
                    isProsodyOpen = false;
                }
            }

            // Check if we need adding a space
            if (frag.TextLength > 0 && !lastGrapheme.empty()) {
                if (NeedAddingSpace(lastGrapheme, nextGrapheme)) {
                    result.SsmlString += u' ';
                }
            }

            if (hasProsody && !isProsodyOpen) {
                result.SsmlString += u"<prosody";
                
                if (frag.Pitch != 0.0f) {
                    result.SsmlString += u" pitch='";
                    result.SsmlString += FormatProsodyValue(frag.Pitch * 5.0f);
                    result.SsmlString += u"'";
                }
                
                if (frag.Volume != 100.0f) {
                    result.SsmlString += u" volume='";
                    result.SsmlString += FormatProsodyValue(frag.Volume - 100.0f);
                    result.SsmlString += u"'";
                }
                
                if (frag.Rate != 0.0f) {
                    float ratePercent = (frag.Rate >= 0.0f) ? (frag.Rate * 20.0f) : (frag.Rate * 20.0f / 3.0f);
                    result.SsmlString += u" rate='";
                    result.SsmlString += FormatProsodyValue(ratePercent);
                    result.SsmlString += u"'";
                }
                
                result.SsmlString += u">";

                isProsodyOpen = true;
                currentPitch = frag.Pitch;
                currentVolume = frag.Volume;
                currentRate = frag.Rate;
            }

            // The text itself
            // We map the beginning of the text
            result.OffsetMap.push_back({ static_cast<uint32_t>(result.SsmlString.length()), frag.OriginalOffset });
            
            AppendEscapedXml(result.SsmlString, frag.Text, frag.TextLength);

            // Update lastGrapheme
            if (frag.TextLength > 0) {
                if (frag.TextLength >= 2 && frag.Text[frag.TextLength - 2] >= 0xD800 && frag.Text[frag.TextLength - 2] <= 0xDBFF && frag.Text[frag.TextLength - 1] >= 0xDC00 && frag.Text[frag.TextLength - 1] <= 0xDFFF) {
                    lastGrapheme.assign(frag.Text + frag.TextLength - 2, 2);
                } else {
                    lastGrapheme.assign(frag.Text + frag.TextLength - 1, 1);
                }
            }
        }
    }

    if (isProsodyOpen) {
        result.SsmlString += u"</prosody>";
    }

    result.SsmlString += u"</speak>";
    return result;
}
