#include "pch.h"
#include "StringUtils.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <combaseapi.h>
#include <cstddef>
#include <limits>

namespace StringUtils
{
    std::string WideToUtf8(const wchar_t* text, std::size_t length)
    {
        if (length == 0)
        {
            return std::string{};
        }

        if (text == nullptr)
        {
            return std::string{};
        }

        if (length > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        {
            return std::string{};
        }

        const int inputLength = static_cast<int>(length);
        const int requiredSize = ::WideCharToMultiByte(
            CP_UTF8,
            0,
            text,
            inputLength,
            nullptr,
            0,
            nullptr,
            nullptr);

        if (requiredSize <= 0)
        {
            return std::string{};
        }

        std::string utf8(static_cast<std::size_t>(requiredSize), '\0');
        const int converted = ::WideCharToMultiByte(
            CP_UTF8,
            0,
            text,
            inputLength,
            utf8.data(),
            requiredSize,
            nullptr,
            nullptr);

        if (converted != requiredSize)
        {
            return std::string{};
        }

        return utf8;
    }

    std::string WideToUtf8(std::wstring_view text)
    {
        return WideToUtf8(text.data(), text.size());
    }

    std::wstring Utf8ToWide(const char* text, std::size_t length)
    {
        if (length == 0)
        {
            return std::wstring{};
        }

        if (text == nullptr)
        {
            return std::wstring{};
        }

        if (length > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        {
            return std::wstring{};
        }

        const int inputLength = static_cast<int>(length);
        const int requiredSize = ::MultiByteToWideChar(
            CP_UTF8,
            0,
            text,
            inputLength,
            nullptr,
            0);

        if (requiredSize <= 0)
        {
            return std::wstring{};
        }

        std::wstring wide(static_cast<std::size_t>(requiredSize), L'\0');
        const int converted = ::MultiByteToWideChar(
            CP_UTF8,
            0,
            text,
            inputLength,
            wide.data(),
            requiredSize);

        if (converted != requiredSize)
        {
            return std::wstring{};
        }

        return wide;
    }

    std::wstring Utf8ToWide(std::string_view text)
    {
        return Utf8ToWide(text.data(), text.size());
    }

    wil::unique_cotaskmem_string Utf8ToCoTaskMemWide(std::string_view text)
    {
        if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        {
            return nullptr;
        }

        if (text.empty())
        {
            wchar_t* emptyBuffer = static_cast<wchar_t*>(::CoTaskMemAlloc(sizeof(wchar_t)));
            if (emptyBuffer == nullptr)
            {
                return nullptr;
            }

            emptyBuffer[0] = L'\0';
            return wil::unique_cotaskmem_string(emptyBuffer);
        }

        if (text.data() == nullptr)
        {
            return nullptr;
        }

        const int inputLength = static_cast<int>(text.size());
        const int wideCharacterCount = ::MultiByteToWideChar(
            CP_UTF8,
            0,
            text.data(),
            inputLength,
            nullptr,
            0);

        if (wideCharacterCount <= 0)
        {
            return nullptr;
        }

        const std::size_t charCountWithNull = static_cast<std::size_t>(wideCharacterCount) + 1;
        if (charCountWithNull > (std::numeric_limits<std::size_t>::max)() / sizeof(wchar_t))
        {
            return nullptr;
        }

        const std::size_t bytesToAllocate = charCountWithNull * sizeof(wchar_t);
        wchar_t* rawBuffer = static_cast<wchar_t*>(::CoTaskMemAlloc(bytesToAllocate));
        if (rawBuffer == nullptr)
        {
            return nullptr;
        }

        wil::unique_cotaskmem_string ownedBuffer(rawBuffer);

        const int converted = ::MultiByteToWideChar(
            CP_UTF8,
            0,
            text.data(),
            inputLength,
            ownedBuffer.get(),
            wideCharacterCount);

        if (converted != wideCharacterCount)
        {
            return nullptr;
        }

        ownedBuffer.get()[wideCharacterCount] = L'\0';
        return ownedBuffer;
    }
}
