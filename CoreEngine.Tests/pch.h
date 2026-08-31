/**
 * @file pch.h
 * @brief Central precompiled header for the CoreEngine.Tests unit test project.
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#pragma warning(push)
#pragma warning(disable: 4996)
#include <sapi.h>
#include <sphelper.h>
#pragma warning(pop)


#include <sapiddk.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <nlohmann/json.hpp>

#include <string>
#include <cmath>
#include <concepts>
#include <limits>
#include <thread>
#include <mutex>
#include <vector>
#include <memory>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <functional>

#include "gtest/gtest.h"

void CoreLog(const wchar_t* fmt, ...) noexcept;
std::vector<std::wstring> GetTestLogs();
void ClearTestLogs();
