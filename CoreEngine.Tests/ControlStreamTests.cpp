#include "pch.h"
#include "TestFixtureBase.h"
#include "ControlPipeTestServer.h"
#include "../CoreEngine/PipeClient.h"
#include "../CoreEngine/SapiEngine.h"

using namespace TestInfrastructure;

#if defined(_DEBUG)
TEST_F(SapiEngineTests, ControlWriteTimeoutIncludesMutexContention)
{
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    client.PauseNextControlWriteAfterLockForTest();

    HRESULT firstWriteResult = E_FAIL;
    std::thread firstWriteThread([&] {
        firstWriteResult = client.SendControlMessage({ {"command", "first"} });
    });
    ThreadJoinGuard firstWriteJoin(firstWriteThread);
    auto releaseControlWrite = wil::scope_exit([&] { client.ReleaseControlWriteForTest(); });
    ASSERT_TRUE(client.WaitForControlWritePauseForTest(1000));

    wil::unique_event secondWriteFinished(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ASSERT_TRUE(secondWriteFinished);
    HRESULT secondWriteResult = E_FAIL;
    std::thread secondWriteThread([&] {
        secondWriteResult = client.SendControlMessage({ {"command", "second"} }, 50);
        SetEvent(secondWriteFinished.get());
    });
    ThreadJoinGuard secondWriteJoin(secondWriteThread);
    auto releaseControlWriteBeforeJoins = wil::scope_exit([&] { client.ReleaseControlWriteForTest(); });

    const DWORD completionBeforeRelease = WaitForSingleObject(secondWriteFinished.get(), 500);
    client.ReleaseControlWriteForTest();
    EXPECT_TRUE(secondWriteJoin.Join(1000));
    EXPECT_TRUE(firstWriteJoin.Join(1000));

    EXPECT_EQ(completionBeforeRelease, WAIT_OBJECT_0)
        << "The finite control-write timeout did not bound mutex contention.";
    EXPECT_EQ(secondWriteResult, HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    EXPECT_EQ(firstWriteResult, S_OK);
}
#endif

TEST_F(SapiEngineTests, ReadControlMessageRetainsSecondJsonLineFromOnePipeRead)
{
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    ASSERT_TRUE(server.WriteControl("{\"event\":\"first\"}\n{\"event\":\"second\"}\n"));

    nlohmann::json first;
    nlohmann::json second;
    ASSERT_EQ(client.ReadControlMessage(first), S_OK);
    ASSERT_EQ(client.ReadControlMessage(second), S_OK);

    EXPECT_EQ(first["event"], "first");
    EXPECT_EQ(second["event"], "second");
}

TEST_F(SapiEngineTests, ReadControlMessageReassemblesFragmentedJsonLine)
{
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);

    std::atomic_bool writesSucceeded{true};
    std::thread writer([&server, &writesSucceeded] {
        writesSucceeded = server.WriteControl("{\"event\":\"");
        writesSucceeded = server.WriteControl("fragmented\"}\n") && writesSucceeded.load();
    });
    ThreadJoinGuard writerJoin(writer);

    nlohmann::json message;
    EXPECT_EQ(client.ReadControlMessage(message), S_OK);
    EXPECT_TRUE(writerJoin.Join(1000));

    ASSERT_TRUE(writesSucceeded);
    EXPECT_EQ(message["event"], "fragmented");
}

TEST_F(SapiEngineTests, ReadControlMessageOffsetInfrastructureSupportsSequentialReads)
{
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    ASSERT_TRUE(server.WriteControl("{\"id\":1}\n{\"id\":2}\n{\"id\":3}\n"));

    nlohmann::json msg1;
    nlohmann::json msg2;
    nlohmann::json msg3;

    ASSERT_EQ(client.ReadControlMessage(msg1), S_OK);
    ASSERT_EQ(client.ReadControlMessage(msg2), S_OK);
    ASSERT_EQ(client.ReadControlMessage(msg3), S_OK);

    EXPECT_EQ(msg1["id"], 1);
    EXPECT_EQ(msg2["id"], 2);
    EXPECT_EQ(msg3["id"], 3);
}

TEST_F(SapiEngineTests, ReadControlMessageHandlesCRLFAndEmptyLines)
{
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    ASSERT_TRUE(server.WriteControl("\r\n{\"msg\":\"hello\"}\r\n\n{\"msg\":\"world\"}\n"));

    nlohmann::json msg1;
    nlohmann::json msg2;

    EXPECT_EQ(client.ReadControlMessage(msg1), S_FALSE);
    EXPECT_TRUE(msg1.is_null());

    ASSERT_EQ(client.ReadControlMessage(msg1), S_OK);
    ASSERT_FALSE(msg1.is_null());
    EXPECT_EQ(msg1["msg"], "hello");

    EXPECT_EQ(client.ReadControlMessage(msg2), S_FALSE);

    ASSERT_EQ(client.ReadControlMessage(msg2), S_OK);
    ASSERT_FALSE(msg2.is_null());
    EXPECT_EQ(msg2["msg"], "world");
}

TEST_F(SapiEngineTests, ReadControlMessageCompactsBufferAndPreservesMessagesAcrossCompactionThreshold)
{
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);

    std::string stream;
    constexpr int totalMsgs = 500;
    for (int i = 0; i < totalMsgs; ++i)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"seq\":%d}\n", i);
        stream += buf;
    }

    std::thread writer([&server, stream]()
    {
        server.WriteControl(stream.c_str());
    });

    for (int i = 0; i < totalMsgs; ++i)
    {
        nlohmann::json msg;
        ASSERT_EQ(client.ReadControlMessage(msg), S_OK) << "Failed at index " << i;
        ASSERT_FALSE(msg.is_null()) << "Null json at index " << i;
        EXPECT_EQ(static_cast<int>(msg["seq"]), i);
    }
    writer.join();
}

TEST_F(SapiEngineTests, ReadControlMessageHandlesLargePayloadAcrossCompactionThreshold)
{
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);

    std::string largeVal(4200, 'x');
    std::string msgStr1 = "{\"data\":\"" + largeVal + "\"}\n";
    std::string msgStr2 = "{\"data\":\"small\"}\n";

    std::thread writer([&server, msgStr1, msgStr2]()
    {
        server.WriteControl((msgStr1 + msgStr2).c_str());
    });

    nlohmann::json msg1;
    nlohmann::json msg2;

    ASSERT_EQ(client.ReadControlMessage(msg1), S_OK);
    ASSERT_FALSE(msg1.is_null());
    EXPECT_EQ(msg1["data"].get<std::string>().size(), 4200u);

    ASSERT_EQ(client.ReadControlMessage(msg2), S_OK);
    ASSERT_FALSE(msg2.is_null());
    EXPECT_EQ(msg2["data"], "small");

    writer.join();
}

TEST_F(SapiEngineTests, ReadControlMessage_InvalidUtf8_RecoversPipe)
{
    ControlPipeTestServer server;
    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);

    std::string invalidUtf8 = "{\"type\":\"event\"}\xff\n";
    server.WriteControl(invalidUtf8);

    nlohmann::json outJson;
    HRESULT hr = client.ReadControlMessage(outJson, 1000);
    EXPECT_NE(hr, S_OK);

    std::string validUtf8 = "{\"type\":\"event\"}\n";
    server.WriteControl(validUtf8);

    hr = client.ReadControlMessage(outJson, 1000);
    EXPECT_EQ(hr, S_OK) << "Should recover and read next message successfully";
}

TEST_F(SapiEngineTests, ReadControlMessage_O_N_LinearSearch)
{
    ControlPipeTestServer server;
    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);

    // Send a message in tiny chunks to test offset search tracking
    std::string chunk(100, ' ');
    for (int i = 0; i < 30; ++i)
    {
        server.WriteControl(chunk);
    }
    server.WriteControl("{\"type\":\"event\"}\n");

    nlohmann::json outJson;
    HRESULT hr = client.ReadControlMessage(outJson, 5000); 
    EXPECT_EQ(hr, S_OK);
    if (!outJson.is_null())
    {
        EXPECT_EQ(outJson["type"], "event");
    }
}

TEST_F(SapiEngineTests, ReadControlMessage_MalformedJson)
{
    ClearTestLogs();
    ControlPipeTestServer server;
    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);

    // Write malformed JSON
    std::string badJson = "{ bad_json ]\n";
    server.WriteControl(badJson);

    nlohmann::json outJson;
    HRESULT hr = client.ReadControlMessage(outJson, 1000);
    EXPECT_TRUE(FAILED(hr));

    auto logs = GetTestLogs();
    bool foundParseError = false;
    for (const auto& log : logs)
    {
        if (log.find(L"JSON Parse Error:") != std::wstring::npos)
        {
            foundParseError = true;
            break;
        }
    }
    EXPECT_TRUE(foundParseError) << "Expected to find a JSON Parse Error log message.";
}
