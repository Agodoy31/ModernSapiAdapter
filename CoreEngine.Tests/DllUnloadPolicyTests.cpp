/**
 * @file DllUnloadPolicyTests.cpp
 * @brief Behavioral tests for the CoreEngine COM unload decision.
 */

#include "pch.h"
#include "../CoreEngine/DllUnloadPolicy.h"

namespace
{

enum class LoggerShutdownBehavior
{
    Succeeds,
    Fails,
    AcquiresModuleLock
};

struct UnloadPolicyTestState
{
    long moduleLockCount = 0;
    int moduleLockReadCount = 0;
    int loggerShutdownCallCount = 0;
};

struct ModuleLockCountReader
{
    UnloadPolicyTestState& state;

    long operator()() noexcept
    {
        ++state.moduleLockReadCount;
        return state.moduleLockCount;
    }
};

[[nodiscard]] constexpr bool DoesLoggerShutdownSucceed(const LoggerShutdownBehavior behavior) noexcept
{
    return behavior != LoggerShutdownBehavior::Fails;
}

struct LoggerShutdown
{
    UnloadPolicyTestState& state;
    LoggerShutdownBehavior behavior;

    bool operator()() noexcept
    {
        ++state.loggerShutdownCallCount;
        if (behavior == LoggerShutdownBehavior::AcquiresModuleLock)
        {
            state.moduleLockCount = 1;
        }

        return DoesLoggerShutdownSucceed(behavior);
    }
};

} // namespace

TEST(DllUnloadPolicyTests, RefusesUnloadWhenTheInitialModuleLockIsHeld)
{
    UnloadPolicyTestState state{.moduleLockCount = 1};

    const HRESULT result = CoreEngine::EvaluateDllUnload(
        ModuleLockCountReader{state},
        LoggerShutdown{state, LoggerShutdownBehavior::Succeeds});

    EXPECT_EQ(result, S_FALSE);
    EXPECT_EQ(state.loggerShutdownCallCount, 0);
}

TEST(DllUnloadPolicyTests, RefusesUnloadWhenLoggerShutdownFails)
{
    UnloadPolicyTestState state;

    const HRESULT result = CoreEngine::EvaluateDllUnload(
        ModuleLockCountReader{state},
        LoggerShutdown{state, LoggerShutdownBehavior::Fails});

    EXPECT_EQ(result, S_FALSE);
    EXPECT_EQ(state.moduleLockReadCount, 1);
    EXPECT_EQ(state.loggerShutdownCallCount, 1);
}

TEST(DllUnloadPolicyTests, RefusesUnloadWhenActivationOccursDuringLoggerShutdown)
{
    UnloadPolicyTestState state;

    const HRESULT result = CoreEngine::EvaluateDllUnload(
        ModuleLockCountReader{state},
        LoggerShutdown{state, LoggerShutdownBehavior::AcquiresModuleLock});

    EXPECT_EQ(result, S_FALSE);
    EXPECT_EQ(state.moduleLockReadCount, 2);
    EXPECT_EQ(state.loggerShutdownCallCount, 1);
}

TEST(DllUnloadPolicyTests, AllowsUnloadWhenLoggerShutdownCompletesWithNoModuleLocks)
{
    UnloadPolicyTestState state;

    const HRESULT result = CoreEngine::EvaluateDllUnload(
        ModuleLockCountReader{state},
        LoggerShutdown{state, LoggerShutdownBehavior::Succeeds});

    EXPECT_EQ(result, S_OK);
    EXPECT_EQ(state.moduleLockReadCount, 2);
    EXPECT_EQ(state.loggerShutdownCallCount, 1);
}
