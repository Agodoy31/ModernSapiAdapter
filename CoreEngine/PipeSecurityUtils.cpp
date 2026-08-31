#include "pch.h"
#include "PipeSecurityUtils.h"
#include <sddl.h>
#include <vector>
#include <wil/resource.h>

namespace PipeSecurityUtils
{

std::wstring GetCurrentUserSidString()
{
    HANDLE tokenRaw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tokenRaw))
    {
        return L"DefaultUser";
    }

    wil::unique_handle token(tokenRaw);
    DWORD bytesRequired = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytesRequired);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytesRequired == 0)
    {
        return L"DefaultUser";
    }

    std::vector<BYTE> tokenBuffer(bytesRequired);
    if (!GetTokenInformation(token.get(), TokenUser, tokenBuffer.data(), bytesRequired, &bytesRequired))
    {
        return L"DefaultUser";
    }

    auto* tokenUser = reinterpret_cast<TOKEN_USER*>(tokenBuffer.data());
    LPWSTR sidText = nullptr;
    if (!ConvertSidToStringSidW(tokenUser->User.Sid, &sidText))
    {
        return L"DefaultUser";
    }

    wil::unique_hlocal_string sid(sidText);
    return sid.get() ? std::wstring(sid.get()) : L"DefaultUser";
}

std::wstring BuildPipePath(
    const std::wstring& pipeName,
    const std::wstring& sid,
    const std::wstring& channel)
{
    return L"\\\\.\\pipe\\" + pipeName + L"\\" + sid + L"\\" + channel;
}

} // namespace PipeSecurityUtils
