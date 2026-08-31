#include "pch.h"
#include "../CoreEngine/AsyncLogger.h"

#if defined(_DEBUG)
namespace
{

[[nodiscard]] std::filesystem::path CoreEngineLogPath()
{
    PWSTR roamingPath = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roamingPath)))
    {
        return {};
    }

    const std::filesystem::path path = std::filesystem::path(roamingPath) /
        L"ModernSapiAdapter" / L"CoreEngine.log";
    CoTaskMemFree(roamingPath);
    return path;
}

[[nodiscard]] std::wstring UniqueMarker(const wchar_t* suffix)
{
    return L"AsyncLoggerTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64()) + L"-" + suffix;
}

[[nodiscard]] bool FileContains(const std::filesystem::path& path, const std::wstring& marker)
{
    std::wifstream stream(path);
    const std::wstring contents{
        std::istreambuf_iterator<wchar_t>(stream),
        std::istreambuf_iterator<wchar_t>()};
    return contents.find(marker) != std::wstring::npos;
}

} // namespace

TEST(AsyncLoggerTests, ShutdownDrainsEveryAcceptedMessage)
{
    auto& logger = AsyncLogger::GetInstance();
    const std::wstring first = UniqueMarker(L"drain-first");
    const std::wstring second = UniqueMarker(L"drain-second");

    logger.Log(first);
    logger.Log(second);

    ASSERT_TRUE(logger.Shutdown());
    EXPECT_TRUE(FileContains(CoreEngineLogPath(), first));
    EXPECT_TRUE(FileContains(CoreEngineLogPath(), second));
}

TEST(AsyncLoggerTests, ConcurrentShutdownCallsAreIdempotent)
{
    auto& logger = AsyncLogger::GetInstance();
    const std::wstring pendingMessage(32 * 1024, L'x');
    for (size_t messageIndex = 0; messageIndex < 16; ++messageIndex)
    {
        logger.Log(pendingMessage);
    }

    wil::unique_event startShutdown(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    wil::unique_event callersReady(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ASSERT_TRUE(startShutdown);
    ASSERT_TRUE(callersReady);
    std::atomic_uint shutdownCallerCount{0};
    std::atomic_bool firstShutdownSucceeded{false};
    std::atomic_bool secondShutdownSucceeded{false};

    const auto shutdownCaller = [&](std::atomic_bool& shutdownSucceeded) {
        if (shutdownCallerCount.fetch_add(1, std::memory_order_acq_rel) + 1 == 2)
        {
            SetEvent(callersReady.get());
        }

        WaitForSingleObject(startShutdown.get(), INFINITE);
        shutdownSucceeded.store(logger.Shutdown(), std::memory_order_release);
    };
    std::thread firstCaller(shutdownCaller, std::ref(firstShutdownSucceeded));
    std::thread secondCaller(shutdownCaller, std::ref(secondShutdownSucceeded));
    auto releaseAndJoinCallers = wil::scope_exit([&] {
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

TEST(AsyncLoggerTests, ShutdownDrainsMessagesFromConcurrentProducers)
{
    constexpr size_t producerCount = 4;
    constexpr size_t messagesPerProducer = 8;
    auto& logger = AsyncLogger::GetInstance();
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
        producers.emplace_back([&logger, &startProducing, producerMarkers = std::move(producerMarkers)] {
            while (!startProducing.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }

            for (const std::wstring& marker : producerMarkers)
            {
                logger.Log(marker);
            }
        });
    }

    startProducing.store(true, std::memory_order_release);
    for (std::thread& producer : producers)
    {
        producer.join();
    }

    ASSERT_TRUE(logger.Shutdown());
    for (const std::wstring& marker : markers)
    {
        EXPECT_TRUE(FileContains(CoreEngineLogPath(), marker));
    }
}

TEST(AsyncLoggerTests, LoggingAfterShutdownStartsANewSession)
{
    auto& logger = AsyncLogger::GetInstance();
    ASSERT_TRUE(logger.Shutdown());
    const std::wstring marker = UniqueMarker(L"restart");

    logger.Log(marker);

    ASSERT_TRUE(logger.Shutdown());
    EXPECT_TRUE(FileContains(CoreEngineLogPath(), marker));
}
#endif
