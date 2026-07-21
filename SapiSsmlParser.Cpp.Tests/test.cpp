#include "pch.h"
#include "../SapiSsmlParser.Cpp/SapiSsmlParser.h"
#include <string>
#include <windows.h>

std::string ToUtf8(const std::u16string& s) {
    if (s.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<LPCWCH>(s.data()), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<LPCWCH>(s.data()), (int)s.size(), &result[0], size, nullptr, nullptr);
    return result;
}
std::string ToUtf8(const char16_t* s) {
    return ToUtf8(std::u16string(s));
}

// Helper to create a default fragment
ProviderSpeechFragment CreateFragment(const char16_t* text, uint32_t action = PROVIDER_ACTION_SPEAK, uint32_t offset = 0) {
    ProviderSpeechFragment frag = {};
    frag.Text = text;
    frag.TextLength = static_cast<uint32_t>(std::char_traits<char16_t>::length(text));
    frag.OriginalOffset = offset;
    frag.Action = action;
    frag.Volume = 100.0f;
    frag.Pitch = 0.0f;
    frag.Rate = 0.0f;
    return frag;
}

TEST(SapiSsmlParserTests, BasicText) {
    ProviderSpeechFragment fragments[] = {
        CreateFragment(u"Hello"),
        CreateFragment(u"world")
    };
    
    auto result = SapiSsmlParser::Parse(fragments, 2, L"en-US");
    EXPECT_EQ(ToUtf8(result.SsmlString), ToUtf8(u"<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xmlns:mstts='http://www.w3.org/2001/mstts' xml:lang='en-US'>Hello world</speak>"));
}

TEST(SapiSsmlParserTests, Bookmarks) {
    ProviderSpeechFragment fragments[] = {
        CreateFragment(u"Test", PROVIDER_ACTION_SPEAK, 0),
        CreateFragment(u"bmk1", PROVIDER_ACTION_BOOKMARK, 4),
        CreateFragment(u"Word", PROVIDER_ACTION_SPEAK, 5)
    };
    
    auto result = SapiSsmlParser::Parse(fragments, 3, L"en-US");
    EXPECT_EQ(ToUtf8(result.SsmlString), ToUtf8(u"<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xmlns:mstts='http://www.w3.org/2001/mstts' xml:lang='en-US'>Test<bookmark mark='bmk1'/> Word</speak>"));
    
    ASSERT_EQ(result.OffsetMap.size(), 3);
    EXPECT_EQ(result.OffsetMap[0].second, 0);
    EXPECT_EQ(result.OffsetMap[1].second, 4);
    EXPECT_EQ(result.OffsetMap[2].second, 5);
}

TEST(SapiSsmlParserTests, ProsodyAdjustments) {
    auto frag1 = CreateFragment(u"Fast");
    frag1.Rate = 5.0f; // +100%
    
    auto frag2 = CreateFragment(u"Slow");
    frag2.Rate = -5.0f; // -33%
    
    auto frag3 = CreateFragment(u"Loud");
    frag3.Volume = 50.0f; // -50%
    
    auto frag4 = CreateFragment(u"High");
    frag4.Pitch = 5.0f; // +25%
    
    ProviderSpeechFragment fragments[] = { frag1, frag2, frag3, frag4 };
    auto result = SapiSsmlParser::Parse(fragments, 4, L"en-US");
    
    std::u16string expected = u"<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xmlns:mstts='http://www.w3.org/2001/mstts' xml:lang='en-US'>"
                              u"<prosody rate='+100%'>Fast</prosody> "
                              u"<prosody rate='-33%'>Slow</prosody> "
                              u"<prosody volume='-50%'>Loud</prosody> "
                              u"<prosody pitch='+25%'>High</prosody></speak>";
                              
    EXPECT_EQ(ToUtf8(result.SsmlString), ToUtf8(expected));
}

TEST(SapiSsmlParserTests, NeedAddingSpaceAlgorithm_AsianCharacters) {
    ProviderSpeechFragment fragments[] = {
        CreateFragment(u"\u79C1"),
        CreateFragment(u"\u306F")
    };
    
    auto result = SapiSsmlParser::Parse(fragments, 2, L"ja-JP");
    
    EXPECT_EQ(ToUtf8(result.SsmlString), ToUtf8(u"<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xmlns:mstts='http://www.w3.org/2001/mstts' xml:lang='ja-JP'>\u79C1\u306F</speak>"));
}

TEST(SapiSsmlParserTests, XmlEscaping) {
    ProviderSpeechFragment fragments[] = {
        CreateFragment(u"<Hello> & \"World'")
    };
    
    auto result = SapiSsmlParser::Parse(fragments, 1, L"en-US");
    EXPECT_EQ(ToUtf8(result.SsmlString), ToUtf8(u"<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xmlns:mstts='http://www.w3.org/2001/mstts' xml:lang='en-US'>&lt;Hello&gt; &amp; &quot;World&apos;</speak>"));
}