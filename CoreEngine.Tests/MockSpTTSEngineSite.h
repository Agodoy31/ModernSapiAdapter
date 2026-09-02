/**
 * @file MockSpTTSEngineSite.h
 * @brief High-fidelity test double implementing SAPI 5 ISpTTSEngineSite and ISpEventSink.
 */

#pragma once

#include <windows.h>
#include <sapi.h>
#include <winrt/base.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <condition_variable>
#include <functional>

struct MockSpTTSEngineSite : winrt::implements<MockSpTTSEngineSite, ISpTTSEngineSite, ISpEventSink>
{
    std::atomic<ULONG> totalBytesWritten = 0;
    std::atomic<ULONG> writeCallCount = 0;
    std::atomic<ULONG> bytesAcceptedAfterRejectedWrite = 0;
    std::atomic_bool rejectNextWrite = false;
    std::atomic<DWORD> writeDelayMs = 0;
    std::atomic<DWORD> actions = SPVES_CONTINUE;
    std::function<DWORD()> getActionsCallback;
    std::mutex eventsMutex;
    std::vector<SPEVENT> receivedEvents;
    std::mutex writesMutex;
    std::condition_variable writeCondition;
    std::vector<ULONG> requestedWriteSizes;
    std::vector<uint8_t> acceptedAudio;

    bool WaitForBytesWritten(ULONG expectedBytes, DWORD timeoutMs = 2000)
    {
        std::unique_lock<std::mutex> lock(writesMutex);
        return writeCondition.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                       [&]
                                       {
                                           return totalBytesWritten.load() >= expectedBytes;
                                       });
    }

    void PauseNextWrite()
    {
        std::lock_guard<std::mutex> lock(m_writePauseMutex);
        m_pauseNextWrite = true;
        m_writePaused = false;
        m_releaseWrite = false;
    }

    bool WaitForWritePause(DWORD timeoutMs)
    {
        std::unique_lock<std::mutex> lock(m_writePauseMutex);
        return m_writePauseCondition.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                              [&]
                                              {
                                                  return m_writePaused;
                                              });
    }

    void ReleaseWrite()
    {
        {
            std::lock_guard<std::mutex> lock(m_writePauseMutex);
            m_pauseNextWrite = false;
            m_releaseWrite = true;
        }
        m_writePauseCondition.notify_all();
    }

    IFACEMETHODIMP AddEvents(const SPEVENT *pEventArray, ULONG ulCount) noexcept override
    {
        std::lock_guard<std::mutex> lock(eventsMutex);
        if (pEventArray && ulCount > 0)
        {
            for (ULONG i = 0; i < ulCount; ++i)
            {
                receivedEvents.push_back(pEventArray[i]);
            }
        }
        return S_OK;
    }

    IFACEMETHODIMP GetEventInterest(ULONGLONG *) noexcept override
    {
        return S_OK;
    }

    IFACEMETHODIMP_(DWORD) GetActions() noexcept override
    {
        return getActionsCallback ? getActionsCallback() : actions.load();
    }

    IFACEMETHODIMP Write(const void *data, ULONG cb, ULONG *pcbWritten) noexcept override
    {
        {
            std::unique_lock<std::mutex> lock(m_writePauseMutex);
            if (m_pauseNextWrite)
            {
                m_pauseNextWrite = false;
                m_writePaused = true;
                m_writePauseCondition.notify_all();
                m_writePauseCondition.wait(lock,
                                           [&]
                                           {
                                               return m_releaseWrite;
                                           });
                m_writePaused = false;
                m_releaseWrite = false;
            }
        }
        const DWORD delay = writeDelayMs.load();
        if (delay > 0)
        {
            Sleep(delay);
        }
        writeCallCount++;
        {
            std::lock_guard<std::mutex> lock(writesMutex);
            requestedWriteSizes.push_back(cb);
        }
        if (rejectNextWrite.exchange(false))
        {
            m_rejectedWriteObserved = true;
            if (pcbWritten)
            {
                *pcbWritten = 0;
            }
            return E_FAIL;
        }
        totalBytesWritten += cb;
        {
            std::lock_guard<std::mutex> lock(writesMutex);
            const auto bytes = static_cast<const uint8_t *>(data);
            acceptedAudio.insert(acceptedAudio.end(), bytes, bytes + cb);
        }
        writeCondition.notify_all();
        if (m_rejectedWriteObserved.load())
        {
            bytesAcceptedAfterRejectedWrite += cb;
        }
        if (pcbWritten)
        {
            *pcbWritten = cb;
        }
        return S_OK;
    }

    ULONG BytesAcceptedAfterRejectedWrite() const noexcept
    {
        return bytesAcceptedAfterRejectedWrite.load();
    }

    IFACEMETHODIMP GetRate(long *) noexcept override
    {
        return S_OK;
    }
    IFACEMETHODIMP GetVolume(USHORT *) noexcept override
    {
        return S_OK;
    }
    IFACEMETHODIMP GetSkipInfo(SPVSKIPTYPE *, long *) noexcept override
    {
        return S_OK;
    }
    IFACEMETHODIMP CompleteSkip(long) noexcept override
    {
        return S_OK;
    }

  private:
    std::atomic_bool m_rejectedWriteObserved = false;
    std::mutex m_writePauseMutex;
    std::condition_variable m_writePauseCondition;
    bool m_pauseNextWrite = false;
    bool m_writePaused = false;
    bool m_releaseWrite = false;
};
