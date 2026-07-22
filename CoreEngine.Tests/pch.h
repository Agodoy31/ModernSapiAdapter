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

#include <unknwn.h>
#include <winrt/base.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <provider_abi.h>

#include <string>
#include <thread>
#include <mutex>
#include <vector>
#include <memory>
#include <atomic>
#include <chrono>

#include "gtest/gtest.h"
