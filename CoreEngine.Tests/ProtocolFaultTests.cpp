#include "pch.h"
#include "TestFixtureBase.h"

using namespace TestInfrastructure;

TEST_F(SapiEngineTests, InvalidCancellationBoundaryFaultsTheWorker)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(7));

    HRESULT cancellationResult = S_OK;
    std::thread cancellationThread(
        [&]
        {
            cancellationResult = fixture.worker->CancelAndDrain();
        });
    ThreadJoinGuard cancellationJoin(cancellationThread);

    std::string cancellationRequest;
    ASSERT_TRUE(fixture.server.ReadControl(cancellationRequest));
    ASSERT_NE(cancellationRequest.find("\"command\":\"cancel\""), std::string::npos);
    ASSERT_TRUE(fixture.server.WriteControl("{\"event\":\"synthesis_cancelled\",\"speak_id\":7}\n"));

    EXPECT_TRUE(cancellationJoin.Join(2000));
    EXPECT_EQ(cancellationResult, E_FAIL);
    EXPECT_TRUE(fixture.worker->IsFaulted());
}

#if defined(_DEBUG)
TEST_F(SapiEngineTests, MisalignedSynthesisCompleteTotalFaultsTheWorker)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(11));

    ASSERT_TRUE(
        fixture.server.WriteControl("{\"event\":\"synthesis_complete\",\"speak_id\":11,\"total_audio_bytes\":1}\n"));

    EXPECT_TRUE(fixture.worker->WaitForFaultForTest(1000));
    EXPECT_TRUE(fixture.worker->IsFaulted());
}

TEST_F(SapiEngineTests, MissingSynthesisCompleteTotalFaultsTheWorker)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(21));

    ASSERT_TRUE(fixture.server.WriteControl("{\"event\":\"synthesis_complete\",\"speak_id\":21}\n"));

    EXPECT_TRUE(fixture.worker->WaitForFaultForTest(1000));
    EXPECT_TRUE(fixture.worker->IsFaulted());
}

TEST_F(SapiEngineTests, NonIntegerSynthesisCompleteTotalFaultsTheWorker)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(22));

    ASSERT_TRUE(
        fixture.server.WriteControl("{\"event\":\"synthesis_complete\",\"speak_id\":22,\"total_audio_bytes\":2.5}\n"));

    EXPECT_TRUE(fixture.worker->WaitForFaultForTest(1000));
    EXPECT_TRUE(fixture.worker->IsFaulted());
}
#endif

TEST_F(SapiEngineTests, IntegralFloatSynthesisCompleteFieldsCompleteTheRequest)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(22));

    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":22.0,\"total_audio_bytes\":0.0}\n"));

    EXPECT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);
    EXPECT_FALSE(fixture.worker->IsFaulted());
}

#if defined(_DEBUG)
TEST_F(SapiEngineTests, DuplicateSynthesisCompleteTotalFaultsTheWorker)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(23));

    ASSERT_TRUE(
        fixture.server.WriteControl("{\"event\":\"synthesis_complete\",\"speak_id\":23,\"total_audio_bytes\":0}\n"));
    ASSERT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);
    ASSERT_TRUE(
        fixture.server.WriteControl("{\"event\":\"synthesis_complete\",\"speak_id\":23,\"total_audio_bytes\":0}\n"));

    EXPECT_TRUE(fixture.worker->WaitForFaultForTest(1000));
    EXPECT_TRUE(fixture.worker->IsFaulted());
}
#endif

TEST_F(SapiEngineTests, MisalignedCancellationTotalFaultsTheWorker)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(12));

    HRESULT cancellationResult = S_OK;
    std::thread cancellationThread(
        [&]
        {
            cancellationResult = fixture.worker->CancelAndDrain();
        });
    ThreadJoinGuard cancellationJoin(cancellationThread);
    std::string cancellationRequest;
    ASSERT_TRUE(fixture.server.ReadControl(cancellationRequest));
    ASSERT_TRUE(
        fixture.server.WriteControl("{\"event\":\"synthesis_cancelled\",\"speak_id\":12,\"audio_bytes_written\":1}\n"));

    EXPECT_TRUE(cancellationJoin.Join(2000));
    EXPECT_EQ(cancellationResult, E_FAIL);
    EXPECT_TRUE(fixture.worker->IsFaulted());
}

TEST_F(SapiEngineTests, RequestErrorFailsUtteranceWithoutKillingProvider)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(44));

    const auto cancelStart = std::chrono::steady_clock::now();

    std::thread serverThread(
        [&]
        {
            fixture.server.WriteControl("{\"event\":\"log\",\"speak_id\":44,\"severity\":\"error\",\"message\":"
                                        "\"Synthesis failed: 400 Bad Request\"}\n");
        });

    HRESULT hr = fixture.worker->WaitUntilFinished(fixture.mockSite.get());
    const auto durationMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - cancelStart).count();

    if (serverThread.joinable())
    {
        serverThread.join();
    }

    EXPECT_TRUE(FAILED(hr));
    EXPECT_LT(durationMs, 200);
    EXPECT_FALSE(fixture.worker->IsFaulted());
}

TEST_F(SapiEngineTests, FatalErrorFaultsSessionAndTriggersRestart)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(45));

    std::thread serverThread(
        [&]
        {
            fixture.server.WriteControl(
                "{\"event\":\"log\",\"speak_id\":45,\"severity\":\"fatal\",\"message\":\"Fatal provider crash\"}\n");
        });

    HRESULT hr = fixture.worker->WaitUntilFinished(fixture.mockSite.get());

    if (serverThread.joinable())
    {
        serverThread.join();
    }

    EXPECT_EQ(hr, E_FAIL);
    EXPECT_TRUE(fixture.worker->IsFaulted());
}

TEST_F(SapiEngineTests, LogEventsDoNotExtendSynthesisInactivityTimeout)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(61));

    HRESULT waitResult = S_OK;
    std::atomic_bool waitReturned{false};
    std::mutex waitMutex;
    std::condition_variable waitCv;
    const auto waitStart = std::chrono::steady_clock::now();
    std::thread waitThread(
        [&]
        {
            waitResult = fixture.worker->WaitUntilFinished(nullptr);
            {
                std::lock_guard<std::mutex> lock(waitMutex);
                waitReturned = true;
            }
            waitCv.notify_all();
        });
    ThreadJoinGuard waitJoin(waitThread);

    std::thread logThread(
        [&]
        {
            auto loopStart = std::chrono::steady_clock::now();
            std::unique_lock<std::mutex> lock(waitMutex);
            while (!waitReturned.load() && std::chrono::steady_clock::now() - loopStart < std::chrono::seconds(3))
            {
                fixture.server.WriteControl(
                    "{\"event\":\"log\",\"speak_id\":61,\"severity\":\"info\",\"message\":\"progress\"}\n");
                waitCv.wait_for(lock, std::chrono::milliseconds(500),
                                [&]
                                {
                                    return waitReturned.load();
                                });
            }
        });

    EXPECT_TRUE(WaitForCondition(
        [&]
        {
            return waitReturned.load();
        },
        2500));
    {
        std::lock_guard<std::mutex> lock(waitMutex);
        waitCv.notify_all();
    }
    logThread.join();

    const bool returnedBeforeCleanup = waitReturned.load();
    const auto elapsedBeforeCleanup = std::chrono::steady_clock::now() - waitStart;
    if (!returnedBeforeCleanup)
    {
        fixture.worker->Stop();
    }
    EXPECT_TRUE(waitJoin.Join(2000));

    EXPECT_TRUE(returnedBeforeCleanup);
    EXPECT_EQ(waitResult, HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    EXPECT_TRUE(fixture.worker->IsFaulted());
    EXPECT_GE(elapsedBeforeCleanup, std::chrono::milliseconds(1300));
    EXPECT_LT(elapsedBeforeCleanup, std::chrono::milliseconds(2500));
}
