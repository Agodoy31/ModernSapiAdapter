#pragma once
#include "pch.h"
#include <type_traits>

class ScopedOverlapped final
{
public:
    ScopedOverlapped() noexcept
    {
        HANDLE eventHandle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!eventHandle)
        {
            m_creationResult = HRESULT_FROM_WIN32(GetLastError());
            return;
        }

        m_event.reset(eventHandle);
        m_value.hEvent = m_event.get();
        m_creationResult = S_OK;
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

static_assert(std::is_nothrow_default_constructible_v<ScopedOverlapped>,
    "ScopedOverlapped must be nothrow default constructible");
