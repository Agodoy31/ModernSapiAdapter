#include "pch.h"
#include "../CoreEngine/PipeSecurityUtils.h"

TEST(PipeSecurityUtilsTests, GetCurrentUserSidStringReturnsNonEmptyString) {
    std::wstring sid = PipeSecurityUtils::GetCurrentUserSidString();
    EXPECT_FALSE(sid.empty());
}

TEST(PipeSecurityUtilsTests, BuildPipePathConstructsCanonicalWin32Path) {
    std::wstring path = PipeSecurityUtils::BuildPipePath(L"MyEngine", L"S-1-5-21-123", L"control");
    EXPECT_EQ(path, L"\\\\.\\pipe\\MyEngine\\S-1-5-21-123\\control");
}
