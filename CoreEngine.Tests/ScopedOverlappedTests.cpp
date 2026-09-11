#include "pch.h"
#include "TestFixtureBase.h"
#include "ScopedOverlapped.h"
#include <type_traits>

TEST(ScopedOverlappedTests, IsNonCopyableAndNonMovable)
{
    EXPECT_FALSE(std::is_copy_constructible_v<ScopedOverlapped>);
    EXPECT_FALSE(std::is_copy_assignable_v<ScopedOverlapped>);
    EXPECT_FALSE(std::is_move_constructible_v<ScopedOverlapped>);
    EXPECT_FALSE(std::is_move_assignable_v<ScopedOverlapped>);
}

TEST(ScopedOverlappedTests, InitializesWithValidEventAndZeroesOtherFields)
{
    ScopedOverlapped scoped;
    
    // Validate HRESULT
    EXPECT_EQ(scoped.CreationResult(), S_OK);

    // Validate OVERLAPPED reference
    OVERLAPPED& overlapped = scoped.Value();
    EXPECT_NE(overlapped.hEvent, nullptr);
    EXPECT_EQ(overlapped.Internal, 0u);
    EXPECT_EQ(overlapped.InternalHigh, 0u);
    EXPECT_EQ(overlapped.Offset, 0u);
    EXPECT_EQ(overlapped.OffsetHigh, 0u);
    
    // Ensure event is manual-reset and initially nonsignaled
    const DWORD waitResult = WaitForSingleObject(overlapped.hEvent, 0);
    EXPECT_EQ(waitResult, WAIT_TIMEOUT);
}
