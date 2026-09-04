#include "pch.h"
#include "TestFixtureBase.h"
#include <sddl.h>

using namespace TestInfrastructure;

TEST_F(SapiEngineTests, PipeClientFailsImmediatelyWhenControlPipeAccessIsDenied)
{
    static std::atomic_uint64_t nextPipeId{0};
    const std::wstring pipeName =
        L"CoreEngineDenied_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(++nextPipeId);
    const std::wstring controlPipePath =
        PipeSecurityUtils::BuildPipePath(pipeName, PipeSecurityUtils::GetCurrentUserSidString(), L"control");

    PSECURITY_DESCRIPTOR securityDescriptor = nullptr;
    ASSERT_TRUE(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(D;;GA;;;WD)", SDDL_REVISION_1,
                                                                     &securityDescriptor, nullptr));
    wil::unique_hlocal securityDescriptorHandle(securityDescriptor);

    SECURITY_ATTRIBUTES securityAttributes = {};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.lpSecurityDescriptor = securityDescriptor;

    wil::unique_handle deniedControlPipe(CreateNamedPipeW(controlPipePath.c_str(), PIPE_ACCESS_DUPLEX,
                                                          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096,
                                                          4096, 0, &securityAttributes));
    ASSERT_TRUE(deniedControlPipe);

    PipeClient client;
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(client.Connect(pipeName, L"definitely-not-a-provider.exe"), HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(100));
}

TEST_F(SapiEngineTests, TimedOutControlReadCancelsItsOverlappedOperationBeforeReturning)
{
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

TEST_F(SapiEngineTests, CompleteOverlappedOperationPreservesBytesIfCompletedDuringCancellation)
{
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);

    char readBuffer[64] = {};
    wil::unique_event readEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ASSERT_TRUE(readEvent);
    OVERLAPPED readOverlapped = {};
    readOverlapped.hEvent = readEvent.get();

    DWORD initialRead = 0;
    BOOL readSuccess = ReadFile(client.ControlPipeHandleForTest(), readBuffer, sizeof(readBuffer), &initialRead, &readOverlapped);
    if (!readSuccess)
    {
        ASSERT_EQ(GetLastError(), static_cast<DWORD>(ERROR_IO_PENDING));
    }

    const std::string payload = "synthesis_ready\n";
    ASSERT_TRUE(server.WriteControl(payload));

    DWORD transferred = 0;
    const HRESULT hr = PipeClient::CompleteOverlappedOperationForTest(
        client.ControlPipeHandleForTest(), readOverlapped, transferred, 0);

    EXPECT_EQ(hr, S_OK);
    EXPECT_EQ(transferred, payload.size());
    EXPECT_EQ(std::string_view(readBuffer, transferred), payload);
}

TEST_F(SapiEngineTests, CompleteOverlappedOperationReturnsTimeoutWhenOperationTrulyCancelled)
{
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);

    char readBuffer[64] = {};
    wil::unique_event readEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ASSERT_TRUE(readEvent);
    OVERLAPPED readOverlapped = {};
    readOverlapped.hEvent = readEvent.get();

    DWORD initialRead = 0;
    BOOL readSuccess = ReadFile(client.ControlPipeHandleForTest(), readBuffer, sizeof(readBuffer), &initialRead, &readOverlapped);
    if (!readSuccess)
    {
        ASSERT_EQ(GetLastError(), static_cast<DWORD>(ERROR_IO_PENDING));
    }

    DWORD transferred = 0;
    const HRESULT hr = PipeClient::CompleteOverlappedOperationForTest(
        client.ControlPipeHandleForTest(), readOverlapped, transferred, 20);

    EXPECT_EQ(hr, HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    EXPECT_EQ(transferred, 0u);
}

TEST_F(SapiEngineTests, ControlRecordTimeoutCoversTheWholeFragmentedMessage)
{
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);

    ASSERT_TRUE(server.WriteControl("{\"response\":\"fragmented_incomplete"));

    nlohmann::json response;
    const auto timeoutStart = std::chrono::steady_clock::now();
    const HRESULT result = client.ReadControlMessage(response, 100);
    const auto elapsed = std::chrono::steady_clock::now() - timeoutStart;

    EXPECT_EQ(result, HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    EXPECT_GE(elapsed, std::chrono::milliseconds(80));
    EXPECT_LT(elapsed, std::chrono::milliseconds(300));
}

TEST_F(SapiEngineTests, PipeDisconnectFaultsActiveWorkerWithoutRetryingForever)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(31));

    HRESULT waitResult = S_OK;
    std::atomic_bool waitReturned{false};
    std::thread waitThread(
        [&]
        {
            waitResult = fixture.worker->WaitUntilFinished(nullptr);
            waitReturned = true;
        });
    ThreadJoinGuard waitJoin(waitThread);

    fixture.server.Disconnect();
    EXPECT_TRUE(WaitForCondition(
        [&]
        {
            return waitReturned.load();
        },
        1000));

    const bool returnedBeforeCleanup = waitReturned.load();
    if (!returnedBeforeCleanup)
    {
        fixture.worker->Stop();
    }
    EXPECT_TRUE(waitJoin.Join(2000));

    EXPECT_TRUE(returnedBeforeCleanup);
    EXPECT_TRUE(FAILED(waitResult));
    EXPECT_TRUE(fixture.worker->IsFaulted());
}

TEST_F(SapiEngineTests, SilentActiveRequestTimesOutInsteadOfHoldingSapiForever)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(32));

    HRESULT waitResult = S_OK;
    std::atomic_bool waitReturned{false};
    const auto waitStart = std::chrono::steady_clock::now();
    std::thread waitThread(
        [&]
        {
            waitResult = fixture.worker->WaitUntilFinished(nullptr);
            waitReturned = true;
        });
    ThreadJoinGuard waitJoin(waitThread);

    EXPECT_TRUE(WaitForCondition(
        [&]
        {
            return waitReturned.load();
        },
        2300));

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
    EXPECT_LT(elapsedBeforeCleanup, std::chrono::milliseconds(2300));
}

TEST_F(SapiEngineTests, StalledTerminalAudioDrainTimesOut)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(60));

    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":60,\"total_audio_bytes\":10000}\n"));

    std::vector<uint8_t> partialAudio(2000, 0x1A);
    ASSERT_TRUE(fixture.server.WriteAudio(partialAudio));

    HRESULT waitResult = S_OK;
    std::atomic_bool waitReturned{false};
    const auto waitStart = std::chrono::steady_clock::now();
    std::thread waitThread(
        [&]
        {
            waitResult = fixture.worker->WaitUntilFinished(fixture.mockSite.get());
            waitReturned = true;
        });
    ThreadJoinGuard waitJoin(waitThread);

    EXPECT_TRUE(WaitForCondition(
        [&]
        {
            return waitReturned.load();
        },
        3000));

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
    EXPECT_LT(elapsedBeforeCleanup, std::chrono::milliseconds(3000));
}

#if defined(_DEBUG)
TEST_F(SapiEngineTests, ControlThreadCreationFailureRollsBackAudioThread)
{
    SpeechWorker::FailNextControlThreadCreationForTest();
    PipeServerWorkerFixture fixture;
    EXPECT_THROW(fixture.Initialize(), std::system_error);
}
#endif
