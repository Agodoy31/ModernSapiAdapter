#include "pch.h"
#include <gtest/gtest.h>
#include "../CoreEngine/SapiEngine.h"

TEST(SapiFragmentSerializationTests, NullFragmentListReturnsEmptyJsonArray)
{
    nlohmann::json result = CSapiEngine::SerializeFragmentsToJson(nullptr);
    ASSERT_TRUE(result.is_array());
    EXPECT_TRUE(result.empty());
}

TEST(SapiFragmentSerializationTests, BookmarkFragmentSerializesCorrectly)
{
    const wchar_t bookmarkText[] = L"bookmark_1";
    SPVTEXTFRAG frag{};
    frag.State.eAction = SPVA_Bookmark;
    frag.pTextStart = bookmarkText;
    frag.ulTextLen = static_cast<ULONG>(wcslen(bookmarkText));
    frag.pNext = nullptr;

    nlohmann::json result = CSapiEngine::SerializeFragmentsToJson(&frag);
    ASSERT_TRUE(result.is_array());
    ASSERT_EQ(result.size(), 1u);
    ASSERT_TRUE(result[0].contains("bookmark"));
    EXPECT_EQ(result[0]["bookmark"], "bookmark_1");
}

TEST(SapiFragmentSerializationTests, SilenceFragmentSerializesCorrectly)
{
    SPVTEXTFRAG frag{};
    frag.State.eAction = SPVA_Silence;
    frag.State.SilenceMSecs = 500;
    frag.pNext = nullptr;

    nlohmann::json result = CSapiEngine::SerializeFragmentsToJson(&frag);
    ASSERT_TRUE(result.is_array());
    ASSERT_EQ(result.size(), 1u);
    ASSERT_TRUE(result[0].contains("silence_ms"));
    EXPECT_EQ(result[0]["silence_ms"], 500u);
}

TEST(SapiFragmentSerializationTests, SpeakTextFragmentSerializesOffsetsAndProsody)
{
    const wchar_t speechText[] = L"Hello world";
    SPVTEXTFRAG frag{};
    frag.State.eAction = SPVA_Speak;
    frag.pTextStart = speechText;
    frag.ulTextLen = static_cast<ULONG>(wcslen(speechText));
    frag.ulTextSrcOffset = 42;
    frag.State.Volume = 80;
    frag.State.PitchAdj.MiddleAdj = 5;
    frag.State.RateAdj = 2;
    frag.pNext = nullptr;

    nlohmann::json result = CSapiEngine::SerializeFragmentsToJson(&frag);
    ASSERT_TRUE(result.is_array());
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0]["text"], "Hello world");
    EXPECT_EQ(result[0]["source_offset"], 42u);
    EXPECT_EQ(result[0]["volume"], 80);
    EXPECT_EQ(result[0]["pitch"], 5);
    EXPECT_EQ(result[0]["rate"], 2);
}

TEST(SapiFragmentSerializationTests, MultipleLinkedFragmentsPreserveSequence)
{
    const wchar_t text1[] = L"First";
    SPVTEXTFRAG frag1{};
    frag1.State.eAction = SPVA_Speak;
    frag1.pTextStart = text1;
    frag1.ulTextLen = static_cast<ULONG>(wcslen(text1));
    frag1.ulTextSrcOffset = 0;
    frag1.State.Volume = 100;

    SPVTEXTFRAG frag2{};
    frag2.State.eAction = SPVA_Silence;
    frag2.State.SilenceMSecs = 250;

    const wchar_t text3[] = L"mark";
    SPVTEXTFRAG frag3{};
    frag3.State.eAction = SPVA_Bookmark;
    frag3.pTextStart = text3;
    frag3.ulTextLen = static_cast<ULONG>(wcslen(text3));

    frag1.pNext = &frag2;
    frag2.pNext = &frag3;
    frag3.pNext = nullptr;

    nlohmann::json result = CSapiEngine::SerializeFragmentsToJson(&frag1);
    ASSERT_TRUE(result.is_array());
    ASSERT_EQ(result.size(), 3u);

    EXPECT_EQ(result[0]["text"], "First");
    EXPECT_EQ(result[1]["silence_ms"], 250u);
    EXPECT_EQ(result[2]["bookmark"], "mark");
}

TEST(SapiFragmentSerializationTests, NonAsciiTextFragmentSerializesCorrectly)
{
    const wchar_t speechText[] = L"\u3053\u3093\u306B\u3061\u306F\u4E16\u754C \xD83D\xDE00 caf\u00E9";
    SPVTEXTFRAG frag{};
    frag.State.eAction = SPVA_Speak;
    frag.pTextStart = speechText;
    frag.ulTextLen = static_cast<ULONG>(wcslen(speechText));
    frag.ulTextSrcOffset = 0;
    frag.State.Volume = 100;
    frag.pNext = nullptr;

    nlohmann::json result = CSapiEngine::SerializeFragmentsToJson(&frag);
    ASSERT_TRUE(result.is_array());
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0]["text"].get<std::string>(), "\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF\xE4\xB8\x96\xE7\x95\x8C \xF0\x9F\x98\x80 caf\xC3\xA9");
}

TEST(SapiFragmentSerializationTests, ExplicitLengthFragmentWithEmbeddedNullSerializesCorrectly)
{
    const wchar_t speechText[] = L"Hello\0World";
    SPVTEXTFRAG frag{};
    frag.State.eAction = SPVA_Speak;
    frag.pTextStart = speechText;
    frag.ulTextLen = 11;
    frag.ulTextSrcOffset = 0;
    frag.State.Volume = 100;
    frag.pNext = nullptr;

    nlohmann::json result = CSapiEngine::SerializeFragmentsToJson(&frag);
    ASSERT_TRUE(result.is_array());
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0]["text"], std::string("Hello\0World", 11));
}

TEST(SapiFragmentSerializationTests, NullTextPointerBookmarkSerializesToNull)
{
    SPVTEXTFRAG frag{};
    frag.State.eAction = SPVA_Bookmark;
    frag.pTextStart = nullptr;
    frag.ulTextLen = 10;
    frag.pNext = nullptr;

    nlohmann::json result = CSapiEngine::SerializeFragmentsToJson(&frag);
    ASSERT_TRUE(result.is_array());
    ASSERT_EQ(result.size(), 1u);
    EXPECT_TRUE(result[0].is_null());
}

TEST(SapiFragmentSerializationTests, NullTextPointerSpeakSerializesProsodyWithoutTextOrOffset)
{
    SPVTEXTFRAG frag{};
    frag.State.eAction = SPVA_Speak;
    frag.pTextStart = nullptr;
    frag.ulTextLen = 10;
    frag.ulTextSrcOffset = 42;
    frag.State.Volume = 75;
    frag.State.PitchAdj.MiddleAdj = -2;
    frag.State.RateAdj = 3;
    frag.pNext = nullptr;

    nlohmann::json result = CSapiEngine::SerializeFragmentsToJson(&frag);
    ASSERT_TRUE(result.is_array());
    ASSERT_EQ(result.size(), 1u);
    EXPECT_FALSE(result[0].contains("text"));
    EXPECT_FALSE(result[0].contains("source_offset"));
    EXPECT_EQ(result[0]["volume"], 75);
    EXPECT_EQ(result[0]["pitch"], -2);
    EXPECT_EQ(result[0]["rate"], 3);
}

TEST(SapiFragmentSerializationTests, ZeroTextLengthBookmarkSerializesToNull)
{
    const wchar_t bookmarkText[] = L"bookmark_zero";
    SPVTEXTFRAG frag{};
    frag.State.eAction = SPVA_Bookmark;
    frag.pTextStart = bookmarkText;
    frag.ulTextLen = 0;
    frag.pNext = nullptr;

    nlohmann::json result = CSapiEngine::SerializeFragmentsToJson(&frag);
    ASSERT_TRUE(result.is_array());
    ASSERT_EQ(result.size(), 1u);
    EXPECT_TRUE(result[0].is_null());
}

TEST(SapiFragmentSerializationTests, ZeroTextLengthSpeakSerializesProsodyWithoutTextOrOffset)
{
    const wchar_t speechText[] = L"Ignored text";
    SPVTEXTFRAG frag{};
    frag.State.eAction = SPVA_Speak;
    frag.pTextStart = speechText;
    frag.ulTextLen = 0;
    frag.ulTextSrcOffset = 15;
    frag.State.Volume = 90;
    frag.State.PitchAdj.MiddleAdj = 1;
    frag.State.RateAdj = -1;
    frag.pNext = nullptr;

    nlohmann::json result = CSapiEngine::SerializeFragmentsToJson(&frag);
    ASSERT_TRUE(result.is_array());
    ASSERT_EQ(result.size(), 1u);
    EXPECT_FALSE(result[0].contains("text"));
    EXPECT_FALSE(result[0].contains("source_offset"));
    EXPECT_EQ(result[0]["volume"], 90);
    EXPECT_EQ(result[0]["pitch"], 1);
    EXPECT_EQ(result[0]["rate"], -1);
}

TEST(SapiFragmentSerializationTests, PronounceActionSerializesTextOffsetsAndProsody)
{
    const wchar_t text[] = L"PronounceMe";
    SPVTEXTFRAG frag{};
    frag.State.eAction = SPVA_Pronounce;
    frag.pTextStart = text;
    frag.ulTextLen = static_cast<ULONG>(wcslen(text));
    frag.ulTextSrcOffset = 10;
    frag.State.Volume = 85;
    frag.State.PitchAdj.MiddleAdj = 2;
    frag.State.RateAdj = 1;
    frag.pNext = nullptr;

    nlohmann::json result = CSapiEngine::SerializeFragmentsToJson(&frag);
    ASSERT_TRUE(result.is_array());
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0]["text"], "PronounceMe");
    EXPECT_EQ(result[0]["source_offset"], 10u);
    EXPECT_EQ(result[0]["volume"], 85);
    EXPECT_EQ(result[0]["pitch"], 2);
    EXPECT_EQ(result[0]["rate"], 1);
}

TEST(SapiFragmentSerializationTests, SpellOutActionSerializesTextOffsetsAndProsody)
{
    const wchar_t text[] = L"SpellOutMe";
    SPVTEXTFRAG frag{};
    frag.State.eAction = SPVA_SpellOut;
    frag.pTextStart = text;
    frag.ulTextLen = static_cast<ULONG>(wcslen(text));
    frag.ulTextSrcOffset = 20;
    frag.State.Volume = 70;
    frag.State.PitchAdj.MiddleAdj = 0;
    frag.State.RateAdj = -2;
    frag.pNext = nullptr;

    nlohmann::json result = CSapiEngine::SerializeFragmentsToJson(&frag);
    ASSERT_TRUE(result.is_array());
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0]["text"], "SpellOutMe");
    EXPECT_EQ(result[0]["source_offset"], 20u);
    EXPECT_EQ(result[0]["volume"], 70);
    EXPECT_EQ(result[0]["pitch"], 0);
    EXPECT_EQ(result[0]["rate"], -2);
}

TEST(SapiFragmentSerializationTests, SectionActionSerializesTextOffsetsAndProsody)
{
    const wchar_t text[] = L"SectionHeading";
    SPVTEXTFRAG frag{};
    frag.State.eAction = SPVA_Section;
    frag.pTextStart = text;
    frag.ulTextLen = static_cast<ULONG>(wcslen(text));
    frag.ulTextSrcOffset = 30;
    frag.State.Volume = 95;
    frag.State.PitchAdj.MiddleAdj = 4;
    frag.State.RateAdj = 0;
    frag.pNext = nullptr;

    nlohmann::json result = CSapiEngine::SerializeFragmentsToJson(&frag);
    ASSERT_TRUE(result.is_array());
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0]["text"], "SectionHeading");
    EXPECT_EQ(result[0]["source_offset"], 30u);
    EXPECT_EQ(result[0]["volume"], 95);
    EXPECT_EQ(result[0]["pitch"], 4);
    EXPECT_EQ(result[0]["rate"], 0);
}

TEST(SapiFragmentSerializationTests, ParseUnknownTagActionSerializesTextOffsetsAndProsody)
{
    const wchar_t text[] = L"<custom-tag>";
    SPVTEXTFRAG frag{};
    frag.State.eAction = SPVA_ParseUnknownTag;
    frag.pTextStart = text;
    frag.ulTextLen = static_cast<ULONG>(wcslen(text));
    frag.ulTextSrcOffset = 40;
    frag.State.Volume = 60;
    frag.State.PitchAdj.MiddleAdj = -3;
    frag.State.RateAdj = 2;
    frag.pNext = nullptr;

    nlohmann::json result = CSapiEngine::SerializeFragmentsToJson(&frag);
    ASSERT_TRUE(result.is_array());
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0]["text"], "<custom-tag>");
    EXPECT_EQ(result[0]["source_offset"], 40u);
    EXPECT_EQ(result[0]["volume"], 60);
    EXPECT_EQ(result[0]["pitch"], -3);
    EXPECT_EQ(result[0]["rate"], 2);
}

TEST(SapiFragmentSerializationTests, UnsupportedActionSerializesToNull)
{
    const wchar_t text[] = L"Ignored";
    SPVTEXTFRAG frag{};
    frag.State.eAction = static_cast<SPVACTIONS>(999);
    frag.pTextStart = text;
    frag.ulTextLen = static_cast<ULONG>(wcslen(text));
    frag.ulTextSrcOffset = 50;
    frag.pNext = nullptr;

    nlohmann::json result = CSapiEngine::SerializeFragmentsToJson(&frag);
    ASSERT_TRUE(result.is_array());
    ASSERT_EQ(result.size(), 1u);
    EXPECT_TRUE(result[0].is_null());
}
