/**
 * @file pch.h
 * @brief Central precompiled header for the CoreEngine proxy router component.
 */

#ifndef PCH_H
#define PCH_H

#define WIN32_LEAN_AND_MEAN

/**
 * Classic COM headers (unknwn.h, sapi.h, sphelper.h) must explicitly precede
 * winrt/base.h so winrt::implements recognizes classic COM interfaces
 * (ISpTTSEngine, ISpObjectWithToken) instead of defaulting exclusively to
 * projected WinRT interface implementation schemas.
 */
#include <unknwn.h>
#include <sapi.h>

#pragma warning(push)
#pragma warning(disable: 4996) // sphelper.h uses deprecated GetVersionExW internally
#include <sphelper.h>
#pragma warning(pop)

#include <wil/cppwinrt.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <nlohmann/json.hpp>

#include <winreg.h>

#include <string>
#include <cmath>
#include <concepts>
#include <limits>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <stdexcept>
#include <vector>
#include <map>
#include <functional>
#include <regex>
#include <cwctype>
#include <fstream>
#include <queue>
#include <condition_variable>
#include <new>
#include <ShlObj.h>

void CoreLog(const wchar_t* fmt, ...) noexcept;

#endif // PCH_H
