/**
 * @file TestFixtureBase.h
 * @brief Common test base fixture and auxiliary sync helpers for GoogleTest suites.
 */

#pragma once

#include "pch.h"
#include <gtest/gtest.h>
#include <winrt/base.h>
#include <vector>
#include <thread>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <memory>
#include <algorithm>
#include "ControlPipeTestServer.h"
#include "MockSpTTSEngineSite.h"
#include "MockSpObjectToken.h"
#include "../CoreEngine/PipeClient.h"
#include "../CoreEngine/SapiEngine.h"
#include "../CoreEngine/SpeechWorker.h"
#include "../CoreEngine/PcmFrameAssembler.h"

namespace TestInfrastructure
{

template <typename Predicate>
[[nodiscard]] inline bool WaitForCondition(Predicate &&pred, DWORD timeoutMs = 1000, DWORD pollIntervalMs = 5)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred())
        {
            return true;
        }
        Sleep(pollIntervalMs);
    }
    return pred();
}

inline std::vector<uint8_t> FeedPcmFragments(PcmFrameAssembler &assembler,
                                             const std::vector<std::vector<uint8_t>> &fragments,
                                             std::vector<size_t> &emittedSizes)
{
    std::vector<uint8_t> output;
    for (const auto &fragment : fragments)
    {
        const auto spans = assembler.Process(fragment.data(), fragment.size());
        for (const auto &span : spans)
        {
            emittedSizes.push_back(span.size);
            output.insert(output.end(), span.data, span.data + span.size);
        }
    }
    return output;
}

class ThreadJoinGuard
{
  public:
    explicit ThreadJoinGuard(std::thread &thread) : m_thread(thread)
    {
    }

    bool Join(DWORD timeoutMs = 5000)
    {
        if (!m_thread.joinable())
        {
            return true;
        }
        if (WaitForSingleObject(reinterpret_cast<HANDLE>(m_thread.native_handle()), timeoutMs) != WAIT_OBJECT_0)
        {
            ADD_FAILURE() << "Thread failed to terminate within timeout";
            m_thread.detach();
            return false;
        }
        m_thread.join();
        return true;
    }

    ~ThreadJoinGuard()
    {
        Join(5000);
    }

  private:
    std::thread &m_thread;
};

class ScopedFaultEventLog
{
  public:
    ScopedFaultEventLog()
    {
        for (int attempt = 0; attempt < 20 && !m_isActive; ++attempt)
        {
            std::error_code error;
            std::filesystem::remove(m_path, error);

            std::ofstream stream(m_path, std::ios::binary | std::ios::trunc);
            m_isActive = stream.good();
            if (!m_isActive)
            {
                Sleep(10);
            }
        }
    }

    ~ScopedFaultEventLog()
    {
        std::error_code error;
        std::filesystem::remove(m_path, error);
    }

    bool IsActive() const noexcept
    {
        return m_isActive;
    }

    size_t SnapshotSize() const
    {
        std::error_code error;
        return std::filesystem::exists(m_path, error) ? static_cast<size_t>(std::filesystem::file_size(m_path, error))
                                                      : 0;
    }

    bool WaitForEntryAfter(size_t snapshotSize, const std::string &entry, DWORD timeoutMs) const
    {
        return WaitForCondition(
            [&]
            {
                std::ifstream stream(m_path, std::ios::binary);
                std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
                return contents.size() >= snapshotSize && contents.find(entry, snapshotSize) != std::string::npos;
            },
            timeoutMs, 10);
    }

  private:
    std::filesystem::path m_path{std::filesystem::temp_directory_path() /
                                 L"ModernSapiAdapterMockProviderFaultTrace.log"};
    bool m_isActive{false};
};

struct PipeServerWorkerFixture
{
    ControlPipeTestServer server;
    PipeClient client;
    winrt::com_ptr<CSapiEngine> engine;
    winrt::com_ptr<MockSpTTSEngineSite> mockSite;
    std::unique_ptr<SpeechWorker> worker;

    bool Initialize(WORD blockAlign = 2)
    {
        if (server.CreateError() != ERROR_SUCCESS)
        {
            return false;
        }
        if (FAILED(client.Connect(server.PipeName(), L"")))
        {
            return false;
        }
        engine = winrt::make_self<CSapiEngine>();
        mockSite = winrt::make_self<MockSpTTSEngineSite>();
        engine->m_cpSite.copy_from(mockSite.get());
        engine->m_config.audioFormat = {
            WAVE_FORMAT_PCM, 1, 24000, 48000, blockAlign, static_cast<WORD>(blockAlign * 8), 0};
        worker = std::make_unique<SpeechWorker>(engine.get(), &client, blockAlign);
        return true;
    }

    bool Start(uint64_t speakId)
    {
        if (!worker)
        {
            return false;
        }
        if (engine)
        {
            std::lock_guard<std::mutex> lock(engine->m_siteMutex);
            engine->m_activeSpeakId = speakId;
        }
        return worker->Start(speakId);
    }

    bool WaitForFault(DWORD timeoutMs = 1000)
    {
        return WaitForCondition(
            [this]
            {
                return worker && worker->IsFaulted();
            },
            timeoutMs);
    }
};

struct EngineInitializedFixture
{
    winrt::com_ptr<MockSpTTSEngineSite> mockSite;
    winrt::com_ptr<CSapiEngine> engine;
    winrt::com_ptr<MockSpObjectToken> mockToken;
    GUID formatId = {};
    WAVEFORMATEX *pWaveFormat = nullptr;

    bool Initialize()
    {
        mockSite = winrt::make_self<MockSpTTSEngineSite>();
        engine = winrt::make_self<CSapiEngine>();
        mockToken = winrt::make_self<MockSpObjectToken>();
        if (FAILED(engine->SetObjectToken(mockToken.get())))
        {
            return false;
        }
        if (FAILED(engine->GetOutputFormat(nullptr, nullptr, &formatId, &pWaveFormat)) || !pWaveFormat)
        {
            return false;
        }
        return true;
    }

    ~EngineInitializedFixture()
    {
        if (pWaveFormat)
        {
            CoTaskMemFree(pWaveFormat);
            pWaveFormat = nullptr;
        }
    }
};

} // namespace TestInfrastructure

class SapiEngineTests : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }
};
