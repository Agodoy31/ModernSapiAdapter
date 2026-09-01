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
