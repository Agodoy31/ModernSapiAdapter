#include "pch.h"
#include "SapiSsmlParser.h"
#include <cmath>
#include <string>

// Helper to determine if a space is needed between two characters
static bool NeedAddingSpace(char16_t lastChar, char16_t nextChar) {
    if (lastChar == u' ' || nextChar == u' ' || lastChar == 0 || nextChar == 0) {
        return false;
    }

    // Check if either character is CJK (Ideograph, Katakana, Hiragana)
    WORD type1 = 0, type2 = 0;
    GetStringTypeW(CT_CTYPE3, reinterpret_cast<LPCWSTR>(&lastChar), 1, &type1);
    GetStringTypeW(CT_CTYPE3, reinterpret_cast<LPCWSTR>(&nextChar), 1, &type2);

    if ((type1 & (C3_IDEOGRAPH | C3_KATAKANA | C3_HIRAGANA)) ||
        (type2 & (C3_IDEOGRAPH | C3_KATAKANA | C3_HIRAGANA))) {
        return false;
    }

    return true;
}

// Helper to escape XML characters
static std::u16string EscapeXml(const char16_t* text, uint32_t length) {
    std::u16string result;
    result.reserve(length + length / 4); // basic heuristic
    for (uint32_t i = 0; i < length; ++i) {
        char16_t c = text[i];
        switch (c) {
            case u'<': result += u"&lt;"; break;
            case u'>': result += u"&gt;"; break;
            case u'&': result += u"&amp;"; break;
            case u'\"': result += u"&quot;"; break;
            case u'\'': result += u"&apos;"; break;
            default: result += c; break;
        }
    }
    return result;
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

    char16_t lastChar = 0;

    for (uint32_t i = 0; i < count; ++i) {
        const auto& frag = fragments[i];

        if (frag.Action == PROVIDER_ACTION_BOOKMARK) {
            result.SsmlString += u"<bookmark mark='";
            // Map the offset to the start of the bookmark
            result.OffsetMap.push_back({ static_cast<uint32_t>(result.SsmlString.length()), frag.OriginalOffset });
            result.SsmlString += EscapeXml(frag.Text, frag.TextLength);
            result.SsmlString += u"'/>";
        } else if (frag.Action == PROVIDER_ACTION_SPEAK || 
                   frag.Action == PROVIDER_ACTION_SPELL_OUT || 
                   frag.Action == PROVIDER_ACTION_PRONOUNCE) {
            
            // Check if we need adding a space
            if (frag.TextLength > 0 && lastChar != 0) {
                char16_t nextChar = frag.Text[0];
                if (NeedAddingSpace(lastChar, nextChar)) {
                    result.SsmlString += u' ';
                }
            }

            bool hasProsody = (frag.Pitch != 0.0f) || (frag.Volume != 100.0f) || (frag.Rate != 0.0f);
            
            if (hasProsody) {
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
            }

            // The text itself
            // We map the beginning of the text
            result.OffsetMap.push_back({ static_cast<uint32_t>(result.SsmlString.length()), frag.OriginalOffset });
            
            std::u16string escapedText = EscapeXml(frag.Text, frag.TextLength);
            result.SsmlString += escapedText;

            if (hasProsody) {
                result.SsmlString += u"</prosody>";
            }

            // Update lastChar
            if (frag.TextLength > 0) {
                lastChar = frag.Text[frag.TextLength - 1];
            }
        }
    }

    result.SsmlString += u"</speak>";
    return result;
}
