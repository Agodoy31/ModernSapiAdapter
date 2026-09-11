#include "pch.h"
#include "TestFixtureBase.h"
#include "ScopedOverlapped.h"
#include <type_traits>

TEST(ScopedOverlappedTests, IsNonCopyableAndNonMovable)
{
    EXPECT_TRUE(std::is_nothrow_default_constructible_v<ScopedOverlapped>);
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

    // Confirm initial zero-time wait returns WAIT_TIMEOUT
    EXPECT_EQ(WaitForSingleObject(overlapped.hEvent, 0), static_cast<DWORD>(WAIT_TIMEOUT));

    // Signal the event and verify success
    EXPECT_TRUE(SetEvent(overlapped.hEvent));

    // Manual-reset contract: consecutive waits must remain signaled (WAIT_OBJECT_0)
    EXPECT_EQ(WaitForSingleObject(overlapped.hEvent, 0), static_cast<DWORD>(WAIT_OBJECT_0));
    EXPECT_EQ(WaitForSingleObject(overlapped.hEvent, 0), static_cast<DWORD>(WAIT_OBJECT_0));

    // Reset the event and verify success
    EXPECT_TRUE(ResetEvent(overlapped.hEvent));

    // Confirm final zero-time wait returns WAIT_TIMEOUT
    EXPECT_EQ(WaitForSingleObject(overlapped.hEvent, 0), static_cast<DWORD>(WAIT_TIMEOUT));
}
