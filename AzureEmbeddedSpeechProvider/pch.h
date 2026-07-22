/**
 * @file pch.h
 * @brief Central precompiled header for the AzureEmbeddedSpeechProvider plugin module.
 */

#ifndef PCH_H
#define PCH_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>

#include <speechapi_cxx.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <fstream>
#include <format>
#include <chrono>
#include <filesystem>
#include <thread>
#include <atomic>
#include <stdexcept>
#include <shlobj.h>

#include <provider_abi.h>
#include "../SapiSsmlParser.Cpp/SapiSsmlParser.h"

#endif // PCH_H
