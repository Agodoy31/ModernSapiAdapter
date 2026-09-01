#pragma once

#include "pch.h"

namespace CoreEngine::Testing
{

#if defined(_DEBUG)

inline void PauseAdmittedDllGetClassObjectPathForTesting() noexcept
{
    wchar_t pausedEventName[128] = {};
    wchar_t releaseEventName[128] = {};
    if (swprintf_s(pausedEventName, L"Local\\ModernSapiAdapter.CoreEngine.DllGetClassObjectPaused.%lu", GetCurrentProcessId()) < 0 ||
        swprintf_s(releaseEventName, L"Local\\ModernSapiAdapter.CoreEngine.DllGetClassObjectRelease.%lu", GetCurrentProcessId()) < 0)
    {
        return;
    }

    wil::unique_event pausedEvent(OpenEventW(EVENT_MODIFY_STATE, FALSE, pausedEventName));
    wil::unique_event releaseEvent(OpenEventW(SYNCHRONIZE, FALSE, releaseEventName));
    if (!pausedEvent || !releaseEvent)
    {
        return;
    }

    SetEvent(pausedEvent.get());
    WaitForSingleObject(releaseEvent.get(), INFINITE);
}

inline void NotifyDllEntryAdmissionClosingForTesting() noexcept
{
    wchar_t closingEventName[128] = {};
    if (swprintf_s(closingEventName, L"Local\\ModernSapiAdapter.CoreEngine.DllEntryClosing.%lu", GetCurrentProcessId()) < 0)
    {
        return;
    }

    wil::unique_event closingEvent(OpenEventW(EVENT_MODIFY_STATE, FALSE, closingEventName));
    if (closingEvent)
    {
        SetEvent(closingEvent.get());
    }
}

#endif

} // namespace CoreEngine::Testing
