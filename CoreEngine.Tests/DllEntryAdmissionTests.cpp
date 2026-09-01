#include "pch.h"
#include "TestFixtureBase.h"
#include "../CoreEngine/DllEntryAdmission.h"

using namespace TestInfrastructure;

TEST(DllEntryAdmissionTests, ClosingWaitsForAnAlreadyAdmittedLease)
{
    DllEntryAdmission admission;
    auto lease = admission.TryEnter();
    ASSERT_TRUE(lease.has_value());

    wil::unique_event closingStarted(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    wil::unique_event closingCompleted(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ASSERT_TRUE(closingStarted);
    ASSERT_TRUE(closingCompleted);
    bool closeSucceeded = false;
    std::thread closer([&] {
        SetEvent(closingStarted.get());
        closeSucceeded = admission.BeginClosingAndWaitForEntries(1000);
        SetEvent(closingCompleted.get());
    });
    ThreadJoinGuard closerJoin(closer);

    ASSERT_EQ(WaitForSingleObject(closingStarted.get(), 1000), WAIT_OBJECT_0);
    EXPECT_EQ(WaitForSingleObject(closingCompleted.get(), 50), WAIT_TIMEOUT);

    lease.reset();
    EXPECT_TRUE(closerJoin.Join());
    EXPECT_TRUE(closeSucceeded);
}

TEST(DllEntryAdmissionTests, ClosingRejectsNewEntriesUntilReopened)
{
    DllEntryAdmission admission;
    ASSERT_TRUE(admission.BeginClosingAndWaitForEntries(0));

    EXPECT_FALSE(admission.TryEnter().has_value());

    admission.Reopen();
    EXPECT_TRUE(admission.TryEnter().has_value());
}

TEST(DllEntryAdmissionTests, TimedOutClosingReopensAdmission)
{
    DllEntryAdmission admission;
    auto lease = admission.TryEnter();
    ASSERT_TRUE(lease.has_value());

    EXPECT_FALSE(admission.BeginClosingAndWaitForEntries(0));
    lease.reset();
    EXPECT_TRUE(admission.TryEnter().has_value());
}
