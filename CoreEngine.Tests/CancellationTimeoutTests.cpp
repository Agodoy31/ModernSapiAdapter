#include "pch.h"
#include "TestFixtureBase.h"

using namespace TestInfrastructure;

TEST_F(SapiEngineTests, SpeakCancelsPromptlyEvenWhenOutputSiteWriteBlocks)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    fixture.mockSite->PauseNextWrite();
    auto releaseWriteGuard = wil::scope_exit([&]
    {
        fixture.mockSite->ReleaseWrite();
    });
    ASSERT_TRUE(fixture.Start(42));

    ASSERT_TRUE(fixture.server.WriteAudio({ 0x10, 0x20, 0x30, 0x40 }));

    ASSERT_TRUE(fixture.mockSite->WaitForWritePause(1000));

    fixture.mockSite->actions = SPVES_ABORT;

    const auto cancelStart = std::chrono::steady_clock::now();

    std::thread serverThread([&]
    {
        std::string request;
        if (fixture.server.ReadControl(request))
        {
            fixture.server.WriteControl("{\"event\":\"synthesis_cancelled\",\"speak_id\":42,\"audio_bytes_written\":4}\n");
        }
    });

    HRESULT cancelHr = fixture.worker->WaitUntilFinished(fixture.mockSite.get());
    const auto cancelDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - cancelStart).count();

    if (serverThread.joinable())
    {
        serverThread.join();
    }

    fixture.mockSite->ReleaseWrite();

    EXPECT_EQ(cancelHr, S_OK);
    EXPECT_LT(cancelDuration, 100);
}

TEST_F(SapiEngineTests, SynthesisCompleteWhileCancellingCompletesPromptly)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(42));

    fixture.mockSite->actions = SPVES_ABORT;

    const auto cancelStart = std::chrono::steady_clock::now();

    std::thread serverThread([&]
    {
        std::string request;
        if (fixture.server.ReadControl(request))
        {
            fixture.server.WriteAudio({ 1, 2, 3, 4 });
            fixture.server.WriteControl("{\"event\":\"synthesis_complete\",\"speak_id\":42,\"total_audio_bytes\":4}\n");
        }
    });

    HRESULT hr = fixture.worker->WaitUntilFinished(fixture.mockSite.get());
    const auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - cancelStart).count();

    if (serverThread.joinable())
    {
        serverThread.join();
    }

    EXPECT_EQ(hr, S_OK);
    EXPECT_LT(durationMs, 200);
}

TEST_F(SapiEngineTests, IgnoredCancellationTimesOutTheEntireTransaction)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(35));

    HRESULT cancellationResult = S_OK;
    std::atomic_bool cancellationReturned{false};
    const auto cancellationStart = std::chrono::steady_clock::now();
    std::thread cancellationThread([&]
    {
        cancellationResult = fixture.worker->CancelAndDrain();
        cancellationReturned = true;
    });
    ThreadJoinGuard cancellationJoin(cancellationThread);

    std::string cancellationRequest;
    ASSERT_TRUE(fixture.server.ReadControl(cancellationRequest));

    EXPECT_TRUE(WaitForCondition([&]
    {
        return cancellationReturned.load();
    }, 1200));

    const bool returnedBeforeAcknowledgement = cancellationReturned.load();
    const auto elapsedBeforeAcknowledgement = std::chrono::steady_clock::now() - cancellationStart;
    if (!returnedBeforeAcknowledgement)
    {
        ASSERT_TRUE(fixture.server.WriteControl(
            "{\"event\":\"synthesis_cancelled\",\"speak_id\":35,\"audio_bytes_written\":0}\n"));
    }
    EXPECT_TRUE(cancellationJoin.Join(2000));

    EXPECT_TRUE(returnedBeforeAcknowledgement);
    EXPECT_EQ(cancellationResult, HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    EXPECT_TRUE(fixture.worker->IsFaulted());
    EXPECT_GE(elapsedBeforeAcknowledgement, std::chrono::milliseconds(400));
    EXPECT_LT(elapsedBeforeAcknowledgement, std::chrono::milliseconds(1200));
}
