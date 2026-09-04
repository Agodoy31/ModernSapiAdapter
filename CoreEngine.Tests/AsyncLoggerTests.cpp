#include "pch.h"
#include "AsyncLoggerTestAccess.h"

#if defined(_DEBUG)
namespace
{

[[nodiscard]] std::filesystem::path CoreEngineLogPath()
{
    static const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                              (L"ModernSapiAdapter_AsyncLoggerTest_" +
                                               std::to_wstring(GetCurrentProcessId()) + L".log");
    AsyncLoggerTestAccess::SetLogFilePath(path.wstring());
    return path;
}

[[nodiscard]] std::wstring UniqueMarker(const wchar_t *suffix)
{
    return L"AsyncLoggerTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) +
           L"-" + suffix;
}

[[nodiscard]] std::string ReadLogTail(const std::filesystem::path &path)
{
    constexpr std::streamoff maximumTailBytes = 4 * 1024 * 1024;
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
    {
        return {};
    }

    const std::streamoff fileSize = stream.tellg();
    if (fileSize < 0)
    {
        return {};
    }

    const std::streamoff readStart = fileSize > maximumTailBytes ? fileSize - maximumTailBytes : 0;
    stream.seekg(readStart, std::ios::beg);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::string MarkerBytes(const std::wstring &marker)
{
    std::string markerBytes;
    markerBytes.reserve(marker.size());
    for (const wchar_t character : marker)
    {
        if (character > L'\x7f')
        {
            ADD_FAILURE() << "AsyncLogger test marker is not ASCII";
            return {};
        }
        markerBytes.push_back(static_cast<char>(character));
    }
    return markerBytes;
}

[[nodiscard]] bool ContainsMarker(const std::vector<std::wstring> &messages, const std::wstring &marker)
{
    for (const std::wstring &message : messages)
    {
        if (message.find(marker) != std::wstring::npos)
        {
            return true;
        }
    }

    return false;
}

} // namespace

class AsyncLoggerTests : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        AsyncLoggerTestAccess::SetLogFilePath(CoreEngineLogPath().wstring());
    }
};

TEST_F(AsyncLoggerTests, ShutdownDrainsEveryAcceptedMessage)
{
    auto *logger = AsyncLogger::GetInstance();
    ASSERT_NE(nullptr, logger);
    const std::wstring first = UniqueMarker(L"drain-first");
    const std::wstring second = UniqueMarker(L"drain-second");

    logger->Log(first);
    logger->Log(second);

    ASSERT_TRUE(logger->Shutdown());
    const std::string logTail = ReadLogTail(CoreEngineLogPath());
    EXPECT_NE(logTail.find(MarkerBytes(first)), std::string::npos);
    EXPECT_NE(logTail.find(MarkerBytes(second)), std::string::npos);
}

TEST_F(AsyncLoggerTests, ConcurrentShutdownCallsAreIdempotent)
{
    auto *logger = AsyncLogger::GetInstance();
    ASSERT_NE(nullptr, logger);
    const std::wstring pendingMessage(32 * 1024, L'x');
    for (size_t messageIndex = 0; messageIndex < 16; ++messageIndex)
    {
        logger->Log(pendingMessage);
    }

    wil::unique_event startShutdown(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    wil::unique_event callersReady(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ASSERT_TRUE(startShutdown);
    ASSERT_TRUE(callersReady);
    std::atomic_uint shutdownCallerCount{0};
    std::atomic_bool firstShutdownSucceeded{false};
    std::atomic_bool secondShutdownSucceeded{false};

    const auto shutdownCaller = [&](std::atomic_bool &shutdownSucceeded)
    {
        if (shutdownCallerCount.fetch_add(1, std::memory_order_acq_rel) + 1 == 2)
        {
            SetEvent(callersReady.get());
        }

        WaitForSingleObject(startShutdown.get(), INFINITE);
        shutdownSucceeded.store(logger->Shutdown(), std::memory_order_release);
    };
    std::thread firstCaller(shutdownCaller, std::ref(firstShutdownSucceeded));
    std::thread secondCaller(shutdownCaller, std::ref(secondShutdownSucceeded));
    auto releaseAndJoinCallers = wil::scope_exit(
        [&]
        {
            SetEvent(startShutdown.get());
            if (firstCaller.joinable())
            {
                firstCaller.join();
            }
            if (secondCaller.joinable())
            {
                secondCaller.join();
            }
        });

    ASSERT_EQ(WaitForSingleObject(callersReady.get(), 1000), WAIT_OBJECT_0);
    ASSERT_TRUE(SetEvent(startShutdown.get()));
    firstCaller.join();
    secondCaller.join();

    EXPECT_TRUE(firstShutdownSucceeded.load(std::memory_order_acquire));
    EXPECT_TRUE(secondShutdownSucceeded.load(std::memory_order_acquire));
}

TEST_F(AsyncLoggerTests, ShutdownDrainsMessagesFromConcurrentProducers)
{
    constexpr size_t producerCount = 4;
    constexpr size_t messagesPerProducer = 8;
    auto *logger = AsyncLogger::GetInstance();
    ASSERT_NE(nullptr, logger);
    std::atomic_bool startProducing{false};
    std::vector<std::wstring> markers;
    markers.reserve(producerCount * messagesPerProducer);
    std::vector<std::thread> producers;
    producers.reserve(producerCount);

    for (size_t producerIndex = 0; producerIndex < producerCount; ++producerIndex)
    {
        std::vector<std::wstring> producerMarkers;
        producerMarkers.reserve(messagesPerProducer);
        for (size_t messageIndex = 0; messageIndex < messagesPerProducer; ++messageIndex)
        {
            producerMarkers.push_back(UniqueMarker(
                (L"producer-" + std::to_wstring(producerIndex) + L"-" + std::to_wstring(messageIndex)).c_str()));
        }

        markers.insert(markers.end(), producerMarkers.begin(), producerMarkers.end());
        producers.emplace_back(
            [logger, &startProducing, producerMarkers = std::move(producerMarkers)]
            {
                while (!startProducing.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }

                for (const std::wstring &marker : producerMarkers)
                {
                    logger->Log(marker);
                }
            });
    }

    startProducing.store(true, std::memory_order_release);
    for (std::thread &producer : producers)
    {
        producer.join();
    }

    ASSERT_TRUE(logger->Shutdown());
    const std::string logTail = ReadLogTail(CoreEngineLogPath());
    for (const std::wstring &marker : markers)
    {
        EXPECT_NE(logTail.find(MarkerBytes(marker)), std::string::npos);
    }
}

TEST_F(AsyncLoggerTests, LoggingAfterShutdownStartsANewSession)
{
    auto *logger = AsyncLogger::GetInstance();
    ASSERT_NE(nullptr, logger);
    ASSERT_TRUE(logger->Shutdown());
    const std::wstring marker = UniqueMarker(L"restart");

    logger->Log(marker);

    ASSERT_TRUE(logger->Shutdown());
    const std::string logTail = ReadLogTail(CoreEngineLogPath());
    EXPECT_NE(logTail.find(MarkerBytes(marker)), std::string::npos);
}

TEST_F(AsyncLoggerTests, QuiescenceRejectsNewMessagesUntilAnUnloadRejectionResumesIt)
{
    auto *logger = AsyncLogger::GetInstance();
    ASSERT_NE(nullptr, logger);
    ASSERT_TRUE(logger->Shutdown());

    std::mutex writtenMessagesMutex;
    std::vector<std::wstring> writtenMessages;
    AsyncLoggerTestAccess::SetWriteCallback(*logger,
                                            [&writtenMessagesMutex, &writtenMessages](const std::wstring &message)
                                            {
                                                std::lock_guard lock(writtenMessagesMutex);
                                                writtenMessages.push_back(message);
                                            });
    auto resetWriteCallback = wil::scope_exit(
        [logger]
        {
            AsyncLoggerTestAccess::SetWriteCallback(*logger, {});
        });

    const std::wstring acceptedBeforeQuiescence = UniqueMarker(L"accepted-before-quiescence");
    const std::wstring rejectedDuringQuiescence = UniqueMarker(L"rejected-during-quiescence");
    const std::wstring acceptedAfterResume = UniqueMarker(L"accepted-after-resume");

    logger->Log(acceptedBeforeQuiescence);
    ASSERT_TRUE(logger->BeginUnloadQuiescence(1000));

    logger->Log(rejectedDuringQuiescence);
    logger->ResumeAfterUnloadRejected();
    logger->Log(acceptedAfterResume);
    ASSERT_TRUE(logger->Shutdown());

    std::lock_guard lock(writtenMessagesMutex);
    EXPECT_TRUE(ContainsMarker(writtenMessages, acceptedBeforeQuiescence));
    EXPECT_FALSE(ContainsMarker(writtenMessages, rejectedDuringQuiescence));
    EXPECT_TRUE(ContainsMarker(writtenMessages, acceptedAfterResume));
}

TEST_F(AsyncLoggerTests, QuiescenceTimeoutReturnsFalseWithoutAbandoningTheWorker)
{
    auto *logger = AsyncLogger::GetInstance();
    ASSERT_NE(nullptr, logger);
    ASSERT_TRUE(logger->Shutdown());

    wil::unique_event writeStarted(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    wil::unique_event releaseWrite(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    wil::unique_event writeCompleted(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ASSERT_TRUE(writeStarted);
    ASSERT_TRUE(releaseWrite);
    ASSERT_TRUE(writeCompleted);
    std::mutex writtenMessagesMutex;
    std::vector<std::wstring> writtenMessages;
    AsyncLoggerTestAccess::SetWriteCallback(*logger,
                                            [&writeStarted, &releaseWrite, &writeCompleted, &writtenMessagesMutex,
                                             &writtenMessages](const std::wstring &message)
                                            {
                                                SetEvent(writeStarted.get());
                                                WaitForSingleObject(releaseWrite.get(), INFINITE);
                                                std::lock_guard lock(writtenMessagesMutex);
                                                writtenMessages.push_back(message);
                                                SetEvent(writeCompleted.get());
                                            });
    auto releaseBlockedWrite = wil::scope_exit(
        [&releaseWrite]
        {
            SetEvent(releaseWrite.get());
        });
    auto resetWriteCallback = wil::scope_exit(
        [logger]
        {
            AsyncLoggerTestAccess::SetWriteCallback(*logger, {});
        });

    const std::wstring blockedMessage = UniqueMarker(L"blocked-message");
    const std::wstring restartedMessage = UniqueMarker(L"restarted-after-timeout");
    logger->Log(blockedMessage);
    ASSERT_EQ(WaitForSingleObject(writeStarted.get(), 1000), WAIT_OBJECT_0);

    EXPECT_FALSE(logger->BeginUnloadQuiescence(50));
    ASSERT_TRUE(SetEvent(releaseWrite.get()));
    ASSERT_EQ(WaitForSingleObject(writeCompleted.get(), 1000), WAIT_OBJECT_0);
    ASSERT_TRUE(AsyncLoggerTestAccess::WaitForWorkerStopped(*logger, 1000));

    logger->Log(restartedMessage);
    ASSERT_TRUE(logger->Shutdown());

    std::lock_guard lock(writtenMessagesMutex);
    EXPECT_TRUE(ContainsMarker(writtenMessages, blockedMessage));
    EXPECT_TRUE(ContainsMarker(writtenMessages, restartedMessage));
}
#endif
