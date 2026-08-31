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
#include "../CoreEngine/PcmFrameAssembler.h"

namespace TestInfrastructure
{

inline std::vector<uint8_t> FeedPcmFragments(PcmFrameAssembler& assembler,
    const std::vector<std::vector<uint8_t>>& fragments,
    std::vector<size_t>& emittedSizes)
{
    std::vector<uint8_t> output;
    for (const auto& fragment : fragments)
    {
        const auto spans = assembler.Process(fragment.data(), fragment.size());
        for (const auto& span : spans)
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
    explicit ThreadJoinGuard(std::thread& thread) : m_thread(thread) {}

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
    std::thread& m_thread;
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

    bool IsActive() const noexcept { return m_isActive; }

    size_t SnapshotSize() const
    {
        std::error_code error;
        return std::filesystem::exists(m_path, error) ? static_cast<size_t>(std::filesystem::file_size(m_path, error)) : 0;
    }

    bool WaitForEntryAfter(size_t snapshotSize, const std::string& entry, DWORD timeoutMs) const
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        do
        {
            std::ifstream stream(m_path, std::ios::binary);
            std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            if (contents.size() >= snapshotSize && contents.find(entry, snapshotSize) != std::string::npos)
            {
                return true;
            }
            Sleep(10);
        } while (std::chrono::steady_clock::now() < deadline);

        return false;
    }

private:
    std::filesystem::path m_path{std::filesystem::temp_directory_path() / L"ModernSapiAdapterMockProviderFaultTrace.log"};
    bool m_isActive{false};
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
