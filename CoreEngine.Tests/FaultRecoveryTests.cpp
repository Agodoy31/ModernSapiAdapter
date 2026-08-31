#include "pch.h"
#include "TestFixtureBase.h"
#include "MockSpTTSEngineSite.h"
#include "MockSpObjectToken.h"
#include "ControlPipeTestServer.h"
#include "../CoreEngine/PipeClient.h"
#include "../CoreEngine/SapiEngine.h"
#include "../CoreEngine/SpeechWorker.h"
#include <sddl.h>

using namespace TestInfrastructure;

TEST_F(SapiEngineTests, PipeClientFailsImmediatelyWhenControlPipeAccessIsDenied) {
    static std::atomic_uint64_t nextPipeId{0};
    const std::wstring pipeName = L"CoreEngineDenied_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
        std::to_wstring(++nextPipeId);
    const std::wstring controlPipePath = PipeSecurityUtils::BuildPipePath(
        pipeName, PipeSecurityUtils::GetCurrentUserSidString(), L"control");

    PSECURITY_DESCRIPTOR securityDescriptor = nullptr;
    ASSERT_TRUE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:(D;;GA;;;WD)", SDDL_REVISION_1, &securityDescriptor, nullptr));
    wil::unique_hlocal securityDescriptorHandle(securityDescriptor);

    SECURITY_ATTRIBUTES securityAttributes = {};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.lpSecurityDescriptor = securityDescriptor;

    wil::unique_handle deniedControlPipe(CreateNamedPipeW(
        controlPipePath.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        4096,
        4096,
        0,
        &securityAttributes));
    ASSERT_TRUE(deniedControlPipe);

    PipeClient client;
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(client.Connect(pipeName, L"definitely-not-a-provider.exe"), HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(100));
}

TEST_F(SapiEngineTests, TimedOutControlReadCancelsItsOverlappedOperationBeforeReturning) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);

    nlohmann::json response;
    const auto timeoutStart = std::chrono::steady_clock::now();
    EXPECT_EQ(client.ReadControlMessage(response, 100), HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    EXPECT_LT(std::chrono::steady_clock::now() - timeoutStart, std::chrono::seconds(1));

    ASSERT_TRUE(server.WriteControl(
        "{\"response\":\"info\",\"audio_format\":{\"sample_rate\":24000,\"bits_per_sample\":16,\"channels\":1}}\n"));
    ASSERT_EQ(client.ReadControlMessage(response, 1000), S_OK);
    ASSERT_FALSE(response.is_null());
    EXPECT_EQ(response["response"], "info");
}

TEST_F(SapiEngineTests, ControlRecordTimeoutCoversTheWholeFragmentedMessage) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);

    std::thread writer([&] {
        server.WriteControl("{\"response\":");
        Sleep(80);
        server.WriteControl("\"info\"");
        Sleep(80);
        server.WriteControl("}\n");
    });
    ThreadJoinGuard writerJoin(writer);

    nlohmann::json response;
    const auto timeoutStart = std::chrono::steady_clock::now();
    const HRESULT result = client.ReadControlMessage(response, 100);
    const auto elapsed = std::chrono::steady_clock::now() - timeoutStart;
    EXPECT_TRUE(writerJoin.Join(1000));

    EXPECT_EQ(result, HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    EXPECT_GE(elapsed, std::chrono::milliseconds(80));
    EXPECT_LT(elapsed, std::chrono::milliseconds(150));
}

TEST_F(SapiEngineTests, PipeDisconnectFaultsActiveWorkerWithoutRetryingForever) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(31));

    HRESULT waitResult = S_OK;
    std::atomic_bool waitReturned{false};
    std::thread waitThread([&] {
        waitResult = worker.WaitUntilFinished(nullptr);
        waitReturned = true;
    });
    ThreadJoinGuard waitJoin(waitThread);

    server.Disconnect();
    for (int attempt = 0; attempt < 100 && !waitReturned.load(); ++attempt)
    {
        Sleep(10);
    }

    const bool returnedBeforeCleanup = waitReturned.load();
    if (!returnedBeforeCleanup)
    {
        worker.Stop();
    }
    EXPECT_TRUE(waitJoin.Join(2000));

    EXPECT_TRUE(returnedBeforeCleanup);
    EXPECT_TRUE(FAILED(waitResult));
    EXPECT_TRUE(worker.IsFaulted());
}

TEST_F(SapiEngineTests, SilentActiveRequestTimesOutInsteadOfHoldingSapiForever) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(32));

    HRESULT waitResult = S_OK;
    std::atomic_bool waitReturned{false};
    const auto waitStart = std::chrono::steady_clock::now();
    std::thread waitThread([&] {
        waitResult = worker.WaitUntilFinished(nullptr);
        waitReturned = true;
    });
    ThreadJoinGuard waitJoin(waitThread);

    const auto cleanupDeadline = waitStart + std::chrono::milliseconds(2300);
    while (!waitReturned.load() && std::chrono::steady_clock::now() < cleanupDeadline)
    {
        Sleep(10);
    }

    const bool returnedBeforeCleanup = waitReturned.load();
    const auto elapsedBeforeCleanup = std::chrono::steady_clock::now() - waitStart;
    if (!returnedBeforeCleanup)
    {
        worker.Stop();
    }
    EXPECT_TRUE(waitJoin.Join(2000));

    EXPECT_TRUE(returnedBeforeCleanup);
    EXPECT_EQ(waitResult, HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    EXPECT_TRUE(worker.IsFaulted());
    EXPECT_GE(elapsedBeforeCleanup, std::chrono::milliseconds(1300));
    EXPECT_LT(elapsedBeforeCleanup, std::chrono::milliseconds(2300));
}

#if defined(_DEBUG)
TEST_F(SapiEngineTests, InvalidSpeechEventSpeakIdQuarantinesTheWorker) {
    const std::vector<std::string> invalidEvents{
        "{\"event\":\"word_boundary\",\"speak_id\":\"40\",\"text_offset\":0,\"text_length\":1,\"audio_offset_ms\":0}\n",
        "{\"event\":\"word_boundary\",\"text_offset\":0,\"text_length\":1,\"audio_offset_ms\":0}\n"
    };

    for (const auto& invalidEvent : invalidEvents)
    {
        ControlPipeTestServer server;
        ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

        PipeClient client;
        ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
        auto engine = winrt::make_self<CSapiEngine>();
        SpeechWorker worker(engine.get(), &client, 2);
        ASSERT_TRUE(worker.Start(40));

        ASSERT_TRUE(server.WriteControl(invalidEvent));

        EXPECT_TRUE(worker.WaitForFaultForTest(1000));
        EXPECT_TRUE(worker.IsFaulted());
    }
}

TEST_F(SapiEngineTests, MalformedRequiredSpeechEventNumbersQuarantineTheWorker) {
    const std::vector<std::string> malformedEvents{
        "{\"event\":\"word_boundary\",\"speak_id\":41,\"text_offset\":\"0\",\"text_length\":1,\"audio_offset_ms\":0}\n",
        "{\"event\":\"sentence_boundary\",\"speak_id\":41,\"text_offset\":0,\"text_length\":1.5,\"audio_offset_ms\":0}\n",
        "{\"event\":\"bookmark_reached\",\"speak_id\":41,\"bookmark_name\":\"mark\"}\n"
    };

    for (const auto& malformedEvent : malformedEvents)
    {
        ControlPipeTestServer server;
        ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

        PipeClient client;
        ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
        auto engine = winrt::make_self<CSapiEngine>();
        SpeechWorker worker(engine.get(), &client, 2);
        ASSERT_TRUE(worker.Start(41));

        ASSERT_TRUE(server.WriteControl(malformedEvent));

        EXPECT_TRUE(worker.WaitForFaultForTest(1000));
        EXPECT_TRUE(worker.IsFaulted());
    }
}
#endif

TEST_F(SapiEngineTests, StaleSpeechEventWithValidSpeakIdDoesNotQuarantineTheWorker) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(43));

    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"word_boundary\",\"speak_id\":42,\"text_offset\":0,\"text_length\":1,\"audio_offset_ms\":0}\n"));
    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":43,\"total_audio_bytes\":0}\n"));

    EXPECT_EQ(worker.WaitUntilFinished(nullptr), S_OK);
    EXPECT_FALSE(worker.IsFaulted());
}

TEST_F(SapiEngineTests, InvalidCancellationBoundaryFaultsTheWorker) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);

    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(7));

    HRESULT cancellationResult = S_OK;
    std::thread cancellationThread([&] {
        cancellationResult = worker.CancelAndDrain();
    });
    ThreadJoinGuard cancellationJoin(cancellationThread);

    std::string cancellationRequest;
    ASSERT_TRUE(server.ReadControl(cancellationRequest));
    ASSERT_NE(cancellationRequest.find("\"command\":\"cancel\""), std::string::npos);
    ASSERT_TRUE(server.WriteControl("{\"event\":\"synthesis_cancelled\",\"speak_id\":7}\n"));

    EXPECT_TRUE(cancellationJoin.Join(2000));
    EXPECT_EQ(cancellationResult, E_FAIL);
    EXPECT_TRUE(worker.IsFaulted());
}

#if defined(_DEBUG)
TEST_F(SapiEngineTests, MisalignedSynthesisCompleteTotalFaultsTheWorker) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(11));

    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":11,\"total_audio_bytes\":1}\n"));

    EXPECT_TRUE(worker.WaitForFaultForTest(1000));
    EXPECT_TRUE(worker.IsFaulted());
}

TEST_F(SapiEngineTests, MissingSynthesisCompleteTotalFaultsTheWorker) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(21));

    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":21}\n"));

    EXPECT_TRUE(worker.WaitForFaultForTest(1000));
    EXPECT_TRUE(worker.IsFaulted());
}

TEST_F(SapiEngineTests, NonIntegerSynthesisCompleteTotalFaultsTheWorker) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(22));

    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":22,\"total_audio_bytes\":2.5}\n"));

    EXPECT_TRUE(worker.WaitForFaultForTest(1000));
    EXPECT_TRUE(worker.IsFaulted());
}
#endif

TEST_F(SapiEngineTests, IntegralFloatSynthesisCompleteFieldsCompleteTheRequest) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(22));

    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":22.0,\"total_audio_bytes\":0.0}\n"));

    EXPECT_EQ(worker.WaitUntilFinished(nullptr), S_OK);
    EXPECT_FALSE(worker.IsFaulted());
}

#if defined(_DEBUG)
TEST_F(SapiEngineTests, DuplicateSynthesisCompleteTotalFaultsTheWorker) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(23));

    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":23,\"total_audio_bytes\":0}\n"));
    ASSERT_EQ(worker.WaitUntilFinished(nullptr), S_OK);
    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":23,\"total_audio_bytes\":0}\n"));

    EXPECT_TRUE(worker.WaitForFaultForTest(1000));
    EXPECT_TRUE(worker.IsFaulted());
}

TEST_F(SapiEngineTests, StaleSynthesisCompleteForDifferentSpeakIdDoesNotFaultIdleWorker) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(32));
    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":32,\"total_audio_bytes\":0}\n"));
    ASSERT_EQ(worker.WaitUntilFinished(nullptr), S_OK);
    worker.PauseNextEventForwardForTest();

    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":31,\"total_audio_bytes\":0}\n"));
    const bool eventPaused = worker.WaitForEventForwardPauseForTest(1000);
    worker.ReleaseEventForwardForTest();

    EXPECT_TRUE(eventPaused);
    EXPECT_FALSE(worker.IsFaulted());
    EXPECT_TRUE(worker.Start(33));
    worker.Stop();
}

TEST_F(SapiEngineTests, AudioAfterNormalCompletionFaultsIdleWorker) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(24));
    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":24,\"total_audio_bytes\":0}\n"));
    ASSERT_EQ(worker.WaitUntilFinished(nullptr), S_OK);

    ASSERT_TRUE(server.WriteAudio({ 0xA1, 0xA2 }));

    EXPECT_TRUE(worker.WaitForFaultForTest(1000));
    EXPECT_TRUE(worker.IsFaulted());
}

TEST_F(SapiEngineTests, FaultPendingRejectsStartBeforeFaultPublicationCompletes) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(28));
    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":28,\"total_audio_bytes\":0}\n"));
    ASSERT_EQ(worker.WaitUntilFinished(nullptr), S_OK);
    worker.PauseNextFaultPublicationForTest();

    ASSERT_TRUE(server.WriteAudio({ 0xE1, 0xE2 }));
    const bool publicationPaused = worker.WaitForFaultPublicationPauseForTest(1000);
    EXPECT_TRUE(publicationPaused);
    EXPECT_TRUE(worker.IsFaulted());
    EXPECT_FALSE(worker.Start(29));
    worker.ReleaseFaultPublicationForTest();

    EXPECT_TRUE(worker.WaitForFaultForTest(1000));
    EXPECT_TRUE(worker.IsFaulted());
}

TEST_F(SapiEngineTests, FrameAssemblyFailureFaultsWorkerWithoutEscapingThread) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    engine->m_cpSite.copy_from(mockSite.get());
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(30));
    worker.FailNextFrameAssemblyForTest();

    ASSERT_TRUE(server.WriteAudio({ 0xF1, 0xF2 }));

    EXPECT_TRUE(worker.WaitForFaultForTest(1000));
    EXPECT_TRUE(worker.IsFaulted());
    EXPECT_FALSE(worker.IsAudioApartmentActiveForTest());
    EXPECT_EQ(mockSite->writeCallCount.load(), 0u);
}

TEST_F(SapiEngineTests, ControlThreadCreationFailureRollsBackAudioThread) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker::FailNextControlThreadCreationForTest();

    EXPECT_THROW(SpeechWorker worker(engine.get(), &client, 2), std::system_error);
}

TEST_F(SapiEngineTests, ControlThreadEntryExceptionFaultsWorkerWithoutEscapingThread) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker::FailNextControlThreadEntryForTest();
    SpeechWorker worker(engine.get(), &client, 2);

    EXPECT_TRUE(worker.WaitForFaultForTest(1000));
    EXPECT_TRUE(worker.IsFaulted());
}

TEST_F(SapiEngineTests, AudioAfterCancellationCompletionFaultsIdleWorker) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(25));

    HRESULT cancellationResult = E_FAIL;
    std::thread cancellationThread([&] { cancellationResult = worker.CancelAndDrain(); });
    ThreadJoinGuard cancellationJoin(cancellationThread);
    std::string cancellationRequest;
    ASSERT_TRUE(server.ReadControl(cancellationRequest));
    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_cancelled\",\"speak_id\":25,\"audio_bytes_written\":0}\n"));
    EXPECT_TRUE(cancellationJoin.Join(2000));
    ASSERT_EQ(cancellationResult, S_OK);

    ASSERT_TRUE(server.WriteAudio({ 0xB1, 0xB2 }));

    EXPECT_TRUE(worker.WaitForFaultForTest(1000));
    EXPECT_TRUE(worker.IsFaulted());
}
#endif

TEST_F(SapiEngineTests, MisalignedCancellationTotalFaultsTheWorker) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(12));

    HRESULT cancellationResult = S_OK;
    std::thread cancellationThread([&] { cancellationResult = worker.CancelAndDrain(); });
    ThreadJoinGuard cancellationJoin(cancellationThread);
    std::string cancellationRequest;
    ASSERT_TRUE(server.ReadControl(cancellationRequest));
    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_cancelled\",\"speak_id\":12,\"audio_bytes_written\":1}\n"));

    EXPECT_TRUE(cancellationJoin.Join(2000));
    EXPECT_EQ(cancellationResult, E_FAIL);
    EXPECT_TRUE(worker.IsFaulted());
}

TEST_F(SapiEngineTests, RequestErrorFailsUtteranceWithoutKillingProvider) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    engine->m_cpSite.copy_from(mockSite.get());

    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(44));

    const auto cancelStart = std::chrono::steady_clock::now();

    std::thread serverThread([&] {
        server.WriteControl("{\"event\":\"log\",\"speak_id\":44,\"severity\":\"error\",\"message\":\"Synthesis failed: 400 Bad Request\"}\n");
    });

    HRESULT hr = worker.WaitUntilFinished(mockSite.get());
    const auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - cancelStart).count();

    if (serverThread.joinable()) {
        serverThread.join();
    }

    EXPECT_TRUE(FAILED(hr));
    EXPECT_LT(durationMs, 200);
    EXPECT_FALSE(worker.IsFaulted());
}

TEST_F(SapiEngineTests, FatalErrorFaultsSessionAndTriggersRestart) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    engine->m_cpSite.copy_from(mockSite.get());

    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(45));

    std::thread serverThread([&] {
        server.WriteControl("{\"event\":\"log\",\"speak_id\":45,\"severity\":\"fatal\",\"message\":\"Fatal provider crash\"}\n");
    });

    HRESULT hr = worker.WaitUntilFinished(mockSite.get());

    if (serverThread.joinable()) {
        serverThread.join();
    }

    EXPECT_EQ(hr, E_FAIL);
    EXPECT_TRUE(worker.IsFaulted());
}
