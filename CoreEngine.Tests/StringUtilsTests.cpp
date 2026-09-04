#include "pch.h"
#include "../CoreEngine/StringUtils.h"
#include <limits>

TEST(StringUtilsTests, WideToUtf8ConvertsAsciiUnicodeAndSupplementaryCharacters)
{
    // ASCII conversion
    EXPECT_EQ(StringUtils::WideToUtf8(L"Hello, world!"), "Hello, world!");
    EXPECT_EQ(StringUtils::WideToUtf8(std::wstring_view(L"Hello, world!")), "Hello, world!");
    EXPECT_EQ(StringUtils::WideToUtf8(L"Hello, world!", 13), "Hello, world!");

    // Non-ASCII BMP conversion (Japanese and accented Latin)
    EXPECT_EQ(StringUtils::WideToUtf8(L"\u3053\u3093\u306B\u3061\u306F\u4E16\u754C"), "\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF\xE4\xB8\x96\xE7\x95\x8C");
    EXPECT_EQ(StringUtils::WideToUtf8(std::wstring_view(L"Caf\u00E9 r\u00E9sum\u00E9")), "Caf\xC3\xA9 r\xC3\xA9sum\xC3\xA9");

    // Supplementary Unicode character (Earth globe U+1F30D: UTF-16 surrogate pair L"\xD83C\xDF0D", UTF-8 "\xF0\x9F\x8C\x8D")
    const wchar_t supplementaryWide[] = L"\xD83C\xDF0D";
    EXPECT_EQ(StringUtils::WideToUtf8(supplementaryWide), "\xF0\x9F\x8C\x8D");
    EXPECT_EQ(StringUtils::WideToUtf8(std::wstring_view(supplementaryWide)), "\xF0\x9F\x8C\x8D");
    EXPECT_EQ(StringUtils::WideToUtf8(supplementaryWide, 2), "\xF0\x9F\x8C\x8D");
}

TEST(StringUtilsTests, Utf8ToWideConvertsAsciiUnicodeAndSupplementaryCharacters)
{
    // ASCII conversion
    EXPECT_EQ(StringUtils::Utf8ToWide("Hello, world!"), L"Hello, world!");
    EXPECT_EQ(StringUtils::Utf8ToWide(std::string_view("Hello, world!")), L"Hello, world!");
    EXPECT_EQ(StringUtils::Utf8ToWide("Hello, world!", 13), L"Hello, world!");

    // Non-ASCII BMP conversion (Japanese and accented Latin)
    EXPECT_EQ(StringUtils::Utf8ToWide("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF\xE4\xB8\x96\xE7\x95\x8C"), L"\u3053\u3093\u306B\u3061\u306F\u4E16\u754C");
    EXPECT_EQ(StringUtils::Utf8ToWide(std::string_view("Caf\xC3\xA9 r\xC3\xA9sum\xC3\xA9")), L"Caf\u00E9 r\u00E9sum\u00E9");

    // Supplementary Unicode character (Earth globe U+1F30D: UTF-8 "\xF0\x9F\x8C\x8D", UTF-16 surrogate pair L"\xD83C\xDF0D")
    const char supplementaryUtf8[] = "\xF0\x9F\x8C\x8D";
    EXPECT_EQ(StringUtils::Utf8ToWide(supplementaryUtf8), L"\xD83C\xDF0D");
    EXPECT_EQ(StringUtils::Utf8ToWide(std::string_view(supplementaryUtf8)), L"\xD83C\xDF0D");
    EXPECT_EQ(StringUtils::Utf8ToWide(supplementaryUtf8, 4), L"\xD83C\xDF0D");
}

TEST(StringUtilsTests, ExplicitLengthOverloadsPreserveEmbeddedNulls)
{
    // Wide to UTF-8 with embedded null
    const wchar_t wideRaw[] = L"abc\0def";
    const size_t wideLength = 7;
    const std::string utf8Result = StringUtils::WideToUtf8(wideRaw, wideLength);
    EXPECT_EQ(utf8Result.size(), 7u);
    EXPECT_EQ(utf8Result[3], '\0');
    EXPECT_EQ(std::string_view(utf8Result.data(), utf8Result.size()), std::string_view("abc\0def", 7));

    const std::wstring_view wideSv(wideRaw, wideLength);
    const std::string utf8FromSv = StringUtils::WideToUtf8(wideSv);
    EXPECT_EQ(utf8FromSv.size(), 7u);
    EXPECT_EQ(utf8FromSv[3], '\0');
    EXPECT_EQ(utf8FromSv, utf8Result);

    // UTF-8 to Wide with embedded null
    const char utf8Raw[] = "ghi\0jkl";
    const size_t utf8Length = 7;
    const std::wstring wideResult = StringUtils::Utf8ToWide(utf8Raw, utf8Length);
    EXPECT_EQ(wideResult.size(), 7u);
    EXPECT_EQ(wideResult[3], L'\0');
    EXPECT_EQ(std::wstring_view(wideResult.data(), wideResult.size()), std::wstring_view(L"ghi\0jkl", 7));

    const std::string_view utf8Sv(utf8Raw, utf8Length);
    const std::wstring wideFromSv = StringUtils::Utf8ToWide(utf8Sv);
    EXPECT_EQ(wideFromSv.size(), 7u);
    EXPECT_EQ(wideFromSv[3], L'\0');
    EXPECT_EQ(wideFromSv, wideResult);
}

TEST(StringUtilsTests, EmptyAndNullInputsFollowTheDocumentedContract)
{
    // Null pointer with length 0
    EXPECT_EQ(StringUtils::WideToUtf8(nullptr, 0), "");
    EXPECT_EQ(StringUtils::Utf8ToWide(nullptr, 0), L"");

    // Null pointer with nonzero length returns empty
    EXPECT_EQ(StringUtils::WideToUtf8(nullptr, 10), "");
    EXPECT_EQ(StringUtils::Utf8ToWide(nullptr, 10), L"");

    // Empty views and empty strings
    EXPECT_EQ(StringUtils::WideToUtf8(std::wstring_view{}), "");
    EXPECT_EQ(StringUtils::WideToUtf8(std::wstring_view(L"")), "");
    EXPECT_EQ(StringUtils::Utf8ToWide(std::string_view{}), L"");
    EXPECT_EQ(StringUtils::Utf8ToWide(std::string_view("")), L"");

    // Empty input for Utf8ToCoTaskMemWide returns a valid allocated NUL-terminated empty string
    auto emptyCoTaskStr = StringUtils::Utf8ToCoTaskMemWide("");
    ASSERT_NE(emptyCoTaskStr.get(), nullptr);
    EXPECT_STREQ(emptyCoTaskStr.get(), L"");
    EXPECT_EQ(wcslen(emptyCoTaskStr.get()), 0u);

    auto emptyCoTaskSv = StringUtils::Utf8ToCoTaskMemWide(std::string_view{});
    ASSERT_NE(emptyCoTaskSv.get(), nullptr);
    EXPECT_STREQ(emptyCoTaskSv.get(), L"");
    EXPECT_EQ(wcslen(emptyCoTaskSv.get()), 0u);
}

TEST(StringUtilsTests, MalformedUnicodeUsesWindowsReplacementBehavior)
{
    // Malformed UTF-8: invalid continuation/leading bytes are replaced with U+FFFD
    const std::string malformedUtf8 = "prefix\x80suffix";
    const std::wstring wideFromMalformed = StringUtils::Utf8ToWide(malformedUtf8);
    EXPECT_FALSE(wideFromMalformed.empty());
    EXPECT_NE(wideFromMalformed.find(L'\uFFFD'), std::wstring::npos);

    // Malformed UTF-16: unpaired high surrogate is replaced with UTF-8 replacement character \xEF\xBF\xBD
    const wchar_t unpairedHigh[] = { L'A', 0xD800, L'B' };
    const std::string utf8FromUnpairedHigh = StringUtils::WideToUtf8(unpairedHigh, 3);
    EXPECT_FALSE(utf8FromUnpairedHigh.empty());
    EXPECT_NE(utf8FromUnpairedHigh.find("\xEF\xBF\xBD"), std::string::npos);

    // Malformed UTF-16: unpaired low surrogate
    const wchar_t unpairedLow[] = { L'X', 0xDC00, L'Y' };
    const std::string utf8FromUnpairedLow = StringUtils::WideToUtf8(unpairedLow, 3);
    EXPECT_FALSE(utf8FromUnpairedLow.empty());
    EXPECT_NE(utf8FromUnpairedLow.find("\xEF\xBF\xBD"), std::string::npos);
}

TEST(StringUtilsTests, UnrepresentableWin32LengthsFailBeforeConversion)
{
    const size_t oversizedLength = static_cast<size_t>((std::numeric_limits<int>::max)()) + 1;

    // A valid 1-character buffer must not be dereferenced beyond bounds
    const wchar_t validWideChar = L'A';
    EXPECT_EQ(StringUtils::WideToUtf8(&validWideChar, oversizedLength), "");

    const char validUtf8Char = 'A';
    EXPECT_EQ(StringUtils::Utf8ToWide(&validUtf8Char, oversizedLength), L"");
}

TEST(StringUtilsTests, Utf8ToCoTaskMemWideReturnsOwnedNullTerminatedUnicode)
{
    const std::string utf8Input = "Hello \xC3\xA9\xE2\x82\xAC \xF0\x9F\x8C\x8D";
    auto coTaskResult = StringUtils::Utf8ToCoTaskMemWide(utf8Input);
    ASSERT_NE(coTaskResult.get(), nullptr);

    const wchar_t* const rawPtr = coTaskResult.get();
    const std::wstring expectedWide = L"Hello \u00E9\u20AC \xD83C\xDF0D";
    EXPECT_STREQ(rawPtr, expectedWide.c_str());

    const size_t expectedLength = expectedWide.size();
    EXPECT_EQ(wcslen(rawPtr), expectedLength);
    EXPECT_EQ(rawPtr[expectedLength], L'\0');

    // Verify ownership transfer and manual CoTaskMemFree
    wchar_t* releasedPtr = coTaskResult.release();
    ASSERT_NE(releasedPtr, nullptr);
    ::CoTaskMemFree(releasedPtr);
}

TEST(StringUtilsTests, Utf8ToCoTaskMemWideHandlesEmptyAndMalformedInputLikeTheExistingPath)
{
    // Empty input produces valid empty string
    auto emptyResult = StringUtils::Utf8ToCoTaskMemWide("");
    ASSERT_NE(emptyResult.get(), nullptr);
    EXPECT_STREQ(emptyResult.get(), L"");
    EXPECT_EQ(emptyResult.get()[0], L'\0');

    // Malformed input produces valid replacement character and is NUL-terminated
    const std::string malformedBookmark = "bookmark_\x80_invalid";
    auto malformedResult = StringUtils::Utf8ToCoTaskMemWide(malformedBookmark);
    ASSERT_NE(malformedResult.get(), nullptr);
    EXPECT_NE(wcschr(malformedResult.get(), 0xFFFD), nullptr);
    EXPECT_EQ(malformedResult.get()[wcslen(malformedResult.get())], L'\0');
}
