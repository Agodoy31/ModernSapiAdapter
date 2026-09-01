#pragma once

#if defined(_DEBUG)
#include <mutex>
#include <condition_variable>
#include <atomic>

/**
 * @struct PipeClientTestHooks
 * @brief Encapsulates debug-only test synchronization and fault injection state for PipeClient.
 */
struct PipeClientTestHooks
{
    std::atomic_bool failNextCancellationMessage{false};
    std::atomic_bool failNextSpeakMessage{false};
    std::mutex controlWriteMutex;
    std::condition_variable controlWriteChanged;
    bool pauseNextControlWriteAfterLock{false};
    bool controlWritePaused{false};
};
#endif
