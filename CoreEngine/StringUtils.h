#pragma once

#include <string>
#include <string_view>
#include <wil/resource.h>

namespace StringUtils
{
    [[nodiscard]] std::string WideToUtf8(const wchar_t* text, size_t length);
    [[nodiscard]] std::string WideToUtf8(std::wstring_view text);
    [[nodiscard]] std::wstring Utf8ToWide(const char* text, size_t length);
    [[nodiscard]] std::wstring Utf8ToWide(std::string_view text);
    [[nodiscard]] wil::unique_cotaskmem_string Utf8ToCoTaskMemWide(std::string_view text);
}
