/**
 * @file PipeSecurityUtils.h
 * @brief Utilities for Win32 named pipe security descriptors and current-user pipe paths.
 */

#pragma once

#include <windows.h>
#include <string>

namespace PipeSecurityUtils
{
    /**
     * @brief Retrieves the current process user SID as a string.
     * @return String SID or L"DefaultUser" on error.
     */
    [[nodiscard]] std::wstring GetCurrentUserSidString();

    /**
     * @brief Constructs the canonical named pipe path for a given pipe name, user SID, and channel.
     * @param pipeName Base name of the pipe.
     * @param sid Current user SID string.
     * @param channel Channel name (e.g. L"control", L"audio").
     * @return Full Win32 named pipe path.
     */
    [[nodiscard]] std::wstring BuildPipePath(
        const std::wstring& pipeName,
        const std::wstring& sid,
        const std::wstring& channel);
}
