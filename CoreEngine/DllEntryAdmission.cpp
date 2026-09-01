#include "DllEntryAdmission.h"

DllEntryAdmission::EntryLease::EntryLease(DllEntryAdmission& admission) noexcept :
    m_admission(&admission)
{
}

DllEntryAdmission::EntryLease::EntryLease(EntryLease&& other) noexcept :
    m_admission(other.m_admission)
{
    other.m_admission = nullptr;
}

DllEntryAdmission::EntryLease& DllEntryAdmission::EntryLease::operator=(EntryLease&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    if (m_admission != nullptr)
    {
        m_admission->Release();
    }

    m_admission = other.m_admission;
    other.m_admission = nullptr;
    return *this;
}

DllEntryAdmission::EntryLease::~EntryLease()
{
    if (m_admission != nullptr)
    {
        m_admission->Release();
    }
}

std::optional<DllEntryAdmission::EntryLease> DllEntryAdmission::TryEnter() noexcept
{
    std::lock_guard lock(m_mutex);
    if (m_state != State::Open)
    {
        return std::nullopt;
    }

    ++m_entryCount;
    return EntryLease(*this);
}

bool DllEntryAdmission::BeginClosingAndWaitForEntries(const DWORD timeoutMs) noexcept
{
    std::unique_lock lock(m_mutex);
    if (m_state != State::Open)
    {
        return false;
    }

    m_state = State::Closing;
    if (m_entryCount == 0)
    {
        return true;
    }

    return m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
        return m_entryCount == 0;
    });
}

void DllEntryAdmission::Reopen() noexcept
{
    std::lock_guard lock(m_mutex);
    if (m_state != State::Closing)
    {
        return;
    }

    m_state = State::Open;
    m_cv.notify_all();
}

void DllEntryAdmission::Release() noexcept
{
    std::lock_guard lock(m_mutex);
    --m_entryCount;
    if (m_entryCount == 0)
    {
        m_cv.notify_all();
    }
}
