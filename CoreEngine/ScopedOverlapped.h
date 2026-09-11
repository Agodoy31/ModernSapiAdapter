#pragma once
#include "pch.h"

class ScopedOverlapped final
{
public:
    ScopedOverlapped() noexcept
    {
        m_event.create(wil::EventOptions::ManualReset);
        if (m_event)
        {
            m_value.hEvent = m_event.get();
            m_creationResult = S_OK;
        }
        else
        {
            m_creationResult = HRESULT_FROM_WIN32(GetLastError());
        }
    }

    ScopedOverlapped(const ScopedOverlapped&) = delete;
    ScopedOverlapped& operator=(const ScopedOverlapped&) = delete;
    ScopedOverlapped(ScopedOverlapped&&) = delete;
    ScopedOverlapped& operator=(ScopedOverlapped&&) = delete;
    ~ScopedOverlapped() = default;

    [[nodiscard]] HRESULT CreationResult() const noexcept
    {
        return m_creationResult;
    }

    [[nodiscard]] OVERLAPPED& Value() noexcept
    {
        return m_value;
    }

private:
    OVERLAPPED m_value{};
    wil::unique_event m_event;
    HRESULT m_creationResult = E_UNEXPECTED;
};
