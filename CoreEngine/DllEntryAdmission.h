#pragma once

#include "pch.h"

class DllEntryAdmission
{
public:
    enum class State
    {
        Open,
        Closing
    };

    class EntryLease
    {
    public:
        EntryLease(const EntryLease&) = delete;
        EntryLease& operator=(const EntryLease&) = delete;
        EntryLease(EntryLease&& other) noexcept;
        EntryLease& operator=(EntryLease&& other) noexcept;
        ~EntryLease();

    private:
        friend class DllEntryAdmission;
        explicit EntryLease(DllEntryAdmission& admission) noexcept;

        DllEntryAdmission* m_admission = nullptr;
    };

    [[nodiscard]] std::optional<EntryLease> TryEnter() noexcept;
    [[nodiscard]] bool BeginClosingAndWaitForEntries(DWORD timeoutMs) noexcept;
    void Reopen() noexcept;

private:
    void Release() noexcept;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    State m_state = State::Open;
    size_t m_entryCount = 0;
};
