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

    static void TearDownTestCase()
    {
        std::error_code ec;
        std::filesystem::remove(CoreEngineLogPath(), ec);
    }

    static void TearDownTestSuite()
    {
        TearDownTestCase();
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

TEST_F(AsyncLoggerTests, CallbackReentrancyIntoLogEnqueuesAndDrainsCleanly)
{
    auto *logger = AsyncLogger::GetInstance();
    ASSERT_NE(nullptr, logger);
    ASSERT_TRUE(logger->Shutdown());

    const std::wstring initialMarker = UniqueMarker(L"reentrancy-initial");
    const std::wstring reenteredMarker = UniqueMarker(L"reentered-marker");

    std::atomic_bool reenteredOnce{false};
    wil::unique_event reenteredDelivered(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ASSERT_TRUE(reenteredDelivered);

    std::mutex writtenMessagesMutex;
    std::vector<std::wstring> writtenMessages;

    AsyncLoggerTestAccess::SetWriteCallback(
        *logger,
        [&](const std::wstring &message)
        {
            if (message.find(initialMarker) != std::wstring::npos &&
                !reenteredOnce.exchange(true))
            {
                logger->Log(reenteredMarker);
            }
            if (message.find(reenteredMarker) != std::wstring::npos)
            {
                SetEvent(reenteredDelivered.get());
            }

            std::lock_guard lock(writtenMessagesMutex);
            writtenMessages.push_back(message);
        });

    auto resetWriteCallback = wil::scope_exit(
        [logger]
        {
            AsyncLoggerTestAccess::SetWriteCallback(*logger, {});
        });

    logger->Log(initialMarker);
    ASSERT_EQ(WaitForSingleObject(reenteredDelivered.get(), 2000), WAIT_OBJECT_0);
    ASSERT_TRUE(logger->Shutdown());

    EXPECT_TRUE(reenteredOnce.load(std::memory_order_acquire));
    std::lock_guard lock(writtenMessagesMutex);
    EXPECT_TRUE(ContainsMarker(writtenMessages, initialMarker));
    EXPECT_TRUE(ContainsMarker(writtenMessages, reenteredMarker));
}

TEST_F(AsyncLoggerTests, RepeatedQuiescenceCallsAreIdempotent)
{
    auto *logger = AsyncLogger::GetInstance();
    ASSERT_NE(nullptr, logger);
    ASSERT_TRUE(logger->Shutdown());

    const std::wstring initialMarker = UniqueMarker(L"idempotent-first");
    const std::wstring rejectedMarker = UniqueMarker(L"idempotent-rejected");
    const std::wstring resumedMarker = UniqueMarker(L"idempotent-resumed");

    std::mutex writtenMessagesMutex;
    std::vector<std::wstring> writtenMessages;

    AsyncLoggerTestAccess::SetWriteCallback(
        *logger,
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

    logger->Log(initialMarker);

    EXPECT_TRUE(logger->BeginUnloadQuiescence(1000));
    EXPECT_TRUE(logger->BeginUnloadQuiescence(1000));
    EXPECT_TRUE(logger->BeginUnloadQuiescence(500));

    logger->Log(rejectedMarker);

    logger->ResumeAfterUnloadRejected();

    logger->Log(resumedMarker);
    ASSERT_TRUE(logger->Shutdown());

    std::lock_guard lock(writtenMessagesMutex);
    EXPECT_TRUE(ContainsMarker(writtenMessages, initialMarker));
    EXPECT_FALSE(ContainsMarker(writtenMessages, rejectedMarker));
    EXPECT_TRUE(ContainsMarker(writtenMessages, resumedMarker));
}

TEST_F(AsyncLoggerTests, CallbackReplacementWhileRunningIsSafe)
{
    auto *logger = AsyncLogger::GetInstance();
    ASSERT_NE(nullptr, logger);
    ASSERT_TRUE(logger->Shutdown());

    constexpr size_t totalMessages = 30;
    constexpr size_t phase1Messages = 10;

    std::mutex deliveryMutex;
    std::vector<std::wstring> callback1Messages;
    std::vector<std::wstring> callback2Messages;

    wil::unique_event callback1Reached(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ASSERT_TRUE(callback1Reached);

    AsyncLoggerTestAccess::SetWriteCallback(
        *logger,
        [&](const std::wstring &message)
        {
            std::lock_guard lock(deliveryMutex);
            callback1Messages.push_back(message);
            if (callback1Messages.size() >= phase1Messages)
            {
                SetEvent(callback1Reached.get());
            }
        });

    auto resetWriteCallback = wil::scope_exit(
        [logger]
        {
            AsyncLoggerTestAccess::SetWriteCallback(*logger, {});
        });

    std::vector<std::wstring> allMarkers;
    allMarkers.reserve(totalMessages);
    for (size_t index = 0; index < totalMessages; ++index)
    {
        allMarkers.push_back(UniqueMarker((L"cb-replace-" + std::to_wstring(index) + L"-token").c_str()));
    }

    for (size_t index = 0; index < phase1Messages; ++index)
    {
        logger->Log(allMarkers[index]);
    }

    ASSERT_EQ(WaitForSingleObject(callback1Reached.get(), 2000), WAIT_OBJECT_0);

    AsyncLoggerTestAccess::SetWriteCallback(
        *logger,
        [&](const std::wstring &message)
        {
            std::lock_guard lock(deliveryMutex);
            callback2Messages.push_back(message);
        });

    for (size_t index = phase1Messages; index < totalMessages; ++index)
    {
        logger->Log(allMarkers[index]);
    }

    ASSERT_TRUE(logger->Shutdown());

    std::lock_guard lock(deliveryMutex);
    EXPECT_GE(callback1Messages.size(), phase1Messages);
    EXPECT_GT(callback2Messages.size(), 0u);
    EXPECT_EQ(callback1Messages.size() + callback2Messages.size(), totalMessages);

    for (const auto &marker : allMarkers)
    {
        const bool in1 = ContainsMarker(callback1Messages, marker);
        const bool in2 = ContainsMarker(callback2Messages, marker);
        EXPECT_TRUE(in1 || in2);
        EXPECT_FALSE(in1 && in2);
    }
}

TEST_F(AsyncLoggerTests, WriteCallbackExceptionDoesNotTerminateWorker)
{
    auto *logger = AsyncLogger::GetInstance();
    ASSERT_NE(nullptr, logger);
    ASSERT_TRUE(logger->Shutdown());

    const std::wstring throwingMarker = UniqueMarker(L"callback-throws");
    const std::wstring survivorMarker = UniqueMarker(L"callback-survivor");

    std::atomic_bool throwTriggered{false};
    std::mutex deliveryMutex;
    std::vector<std::wstring> deliveredMessages;

    AsyncLoggerTestAccess::SetWriteCallback(
        *logger,
        [&](const std::wstring &message)
        {
            if (message.find(throwingMarker) != std::wstring::npos)
            {
                throwTriggered.store(true, std::memory_order_release);
                throw std::runtime_error("Simulated callback exception");
            }

            std::lock_guard lock(deliveryMutex);
            deliveredMessages.push_back(message);
        });

    auto resetWriteCallback = wil::scope_exit(
        [logger]
        {
            AsyncLoggerTestAccess::SetWriteCallback(*logger, {});
        });

    logger->Log(throwingMarker);
    logger->Log(survivorMarker);

    ASSERT_TRUE(logger->Shutdown());

    EXPECT_TRUE(throwTriggered.load(std::memory_order_acquire));
    std::lock_guard lock(deliveryMutex);
    EXPECT_FALSE(ContainsMarker(deliveredMessages, throwingMarker));
    EXPECT_TRUE(ContainsMarker(deliveredMessages, survivorMarker));
}

TEST_F(AsyncLoggerTests, LateWorkerCompletionAfterTimeoutReopensAdmissionViaResumeWhenStopped)
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

    AsyncLoggerTestAccess::SetWriteCallback(
        *logger,
        [&](const std::wstring &message)
        {
            if (message.find(L"late-blocked") != std::wstring::npos)
            {
                SetEvent(writeStarted.get());
                WaitForSingleObject(releaseWrite.get(), INFINITE);
                SetEvent(writeCompleted.get());
            }

            std::lock_guard lock(writtenMessagesMutex);
            writtenMessages.push_back(message);
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

    const std::wstring blockedMessage = UniqueMarker(L"late-blocked-message");
    const std::wstring reopenedMessage = UniqueMarker(L"reopened-without-explicit-resume");

    logger->Log(blockedMessage);
    ASSERT_EQ(WaitForSingleObject(writeStarted.get(), 2000), WAIT_OBJECT_0);

    EXPECT_FALSE(logger->BeginUnloadQuiescence(50));

    ASSERT_TRUE(SetEvent(releaseWrite.get()));
    ASSERT_EQ(WaitForSingleObject(writeCompleted.get(), 2000), WAIT_OBJECT_0);
    ASSERT_TRUE(AsyncLoggerTestAccess::WaitForWorkerStopped(*logger, 2000));

    logger->Log(reopenedMessage);
    ASSERT_TRUE(logger->Shutdown());

    std::lock_guard lock(writtenMessagesMutex);
    EXPECT_TRUE(ContainsMarker(writtenMessages, blockedMessage));
    EXPECT_TRUE(ContainsMarker(writtenMessages, reopenedMessage));
}
#endif
