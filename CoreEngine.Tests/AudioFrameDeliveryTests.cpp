#include "pch.h"
#include "TestFixtureBase.h"

using namespace TestInfrastructure;

#if defined(_DEBUG)
TEST_F(SapiEngineTests, WorkerReassemblesAwkward24BitStereoPipeFragments)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize(6));
    ASSERT_TRUE(fixture.Start(27));

    ASSERT_TRUE(fixture.server.WriteAudio({0x01}));
    EXPECT_TRUE(WaitForCondition(
        [&]
        {
            return fixture.worker->RawAudioBytesForTest() == 1;
        },
        1000, 1));
    ASSERT_EQ(fixture.worker->RawAudioBytesForTest(), 1u);

    ASSERT_TRUE(fixture.server.WriteAudio({0x02, 0x03, 0x04, 0x05}));
    EXPECT_TRUE(WaitForCondition(
        [&]
        {
            return fixture.worker->RawAudioBytesForTest() == 5;
        },
        1000, 1));
    ASSERT_EQ(fixture.worker->RawAudioBytesForTest(), 5u);

    ASSERT_TRUE(fixture.server.WriteAudio({0x06, 0x11, 0x12}));
    EXPECT_TRUE(WaitForCondition(
        [&]
        {
            return fixture.worker->RawAudioBytesForTest() == 8;
        },
        1000, 1));
    ASSERT_EQ(fixture.worker->RawAudioBytesForTest(), 8u);

    ASSERT_TRUE(fixture.server.WriteAudio({0x13, 0x14, 0x15, 0x16, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26}));
    EXPECT_TRUE(WaitForCondition(
        [&]
        {
            return fixture.worker->RawAudioBytesForTest() == 18;
        },
        1000, 1));
    ASSERT_EQ(fixture.worker->RawAudioBytesForTest(), 18u);

    ASSERT_TRUE(
        fixture.server.WriteControl("{\"event\":\"synthesis_complete\",\"speak_id\":27,\"total_audio_bytes\":18}\n"));
    ASSERT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);

    std::lock_guard<std::mutex> lock(fixture.mockSite->writesMutex);
    ASSERT_FALSE(fixture.mockSite->requestedWriteSizes.empty());
    EXPECT_TRUE(std::all_of(fixture.mockSite->requestedWriteSizes.begin(), fixture.mockSite->requestedWriteSizes.end(),
                            [](ULONG requestedSize)
                            {
                                return requestedSize % 6 == 0;
                            }));
    EXPECT_EQ(fixture.mockSite->acceptedAudio,
              (std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x21, 0x22,
                                    0x23, 0x24, 0x25, 0x26}));
}
#endif

#if defined(_DEBUG)
TEST_F(SapiEngineTests, StaleSpansAreDroppedBetweenBatchWrites)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(60));

    fixture.mockSite->PauseNextWrite();
    auto releaseWriteGuard = wil::scope_exit(
        [&]
        {
            fixture.mockSite->ReleaseWrite();
        });

    ASSERT_TRUE(fixture.server.WriteAudio({0x11}));

    // Wait for the worker to read the first byte and put it into carry
    EXPECT_TRUE(WaitForCondition(
        [&]
        {
            return fixture.worker->RawAudioBytesForTest() >= 1;
        },
        1000, 1));

    ASSERT_TRUE(fixture.server.WriteAudio({0x22, 0x33, 0x44}));
    ASSERT_TRUE(fixture.mockSite->WaitForWritePause(1000));

    fixture.worker->Stop();
    ASSERT_TRUE(fixture.worker->Start(61));

    fixture.mockSite->ReleaseWrite();

    ASSERT_TRUE(fixture.server.WriteAudio({0xAA, 0xBB, 0xCC, 0xDD}));
    ASSERT_TRUE(
        fixture.server.WriteControl("{\"event\":\"synthesis_complete\",\"speak_id\":61,\"total_audio_bytes\":4}\n"));

    EXPECT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);
    EXPECT_FALSE(fixture.worker->IsFaulted());

    {
        std::lock_guard<std::mutex> lock(fixture.mockSite->writesMutex);
        EXPECT_EQ(fixture.mockSite->acceptedAudio, (std::vector<uint8_t>{0x11, 0x22, 0xAA, 0xBB, 0xCC, 0xDD}));
    }
}
#endif
