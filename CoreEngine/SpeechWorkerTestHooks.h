#pragma once

#if defined(_DEBUG)
#include <mutex>
#include <condition_variable>
#include <atomic>

/**
 * @struct SpeechWorkerTestHooks
 * @brief Encapsulates debug-only test synchronization, fault injection, and apartment tracking for SpeechWorker.
 */
struct SpeechWorkerTestHooks
{
    std::mutex eventForwardMutex;
    std::condition_variable eventForwardChanged;
    bool pauseNextEventForward{false};
    bool eventForwardPaused{false};

    std::mutex abortTransitionMutex;
    std::condition_variable abortTransitionChanged;
    bool pauseNextAbortTransition{false};
    bool abortTransitionPaused{false};
    bool wasCancellingAtAbortUnlock{false};

    std::mutex faultPublicationMutex;
    std::condition_variable faultPublicationChanged;
    bool pauseNextFaultPublication{false};
    bool faultPublicationPaused{false};

    bool failNextFrameAssembly{false};
    std::atomic_bool audioApartmentActive{false};
};
#endif
