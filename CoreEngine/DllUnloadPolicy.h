/**
 * @file DllUnloadPolicy.h
 * @brief Defines the COM module unload decision contract.
 */

#pragma once

#include <windows.h>

namespace CoreEngine
{

template <typename ModuleLockCount>
[[nodiscard]] constexpr bool IsModuleLockCountZero(const ModuleLockCount& moduleLockCount)
    noexcept(noexcept(moduleLockCount == 0))
{
    return moduleLockCount == 0;
}

template <typename ModuleLockCountReader, typename LoggerShutdown>
[[nodiscard]] HRESULT EvaluateDllUnload(
    ModuleLockCountReader&& moduleLockCountReader,
    LoggerShutdown&& loggerShutdown)
    noexcept(noexcept(moduleLockCountReader()) && noexcept(loggerShutdown()))
{
    if (!IsModuleLockCountZero(moduleLockCountReader()))
    {
        return S_FALSE;
    }

    if (!loggerShutdown())
    {
        return S_FALSE;
    }

    if (!IsModuleLockCountZero(moduleLockCountReader()))
    {
        return S_FALSE;
    }

    return S_OK;
}

} // namespace CoreEngine
