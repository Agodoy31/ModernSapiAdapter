#include "pch.h"
#include "ConfigParser.h"
#include "Logger.h"
#include <set>

using json = nlohmann::json;

std::filesystem::path ConfigParser::GetModuleDirectory() {
    HMODULE hModule = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&ConfigParser::LoadMergedConfig, &hModule);
    WCHAR szPath[MAX_PATH] = { 0 };
    GetModuleFileNameW(hModule, szPath, MAX_PATH);
    std::filesystem::path path(szPath);
    return path.parent_path();
}

std::filesystem::path ConfigParser::GetUserConfigDirectory() {
    PWSTR appData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appData))) {
        std::filesystem::path p = appData;
        CoTaskMemFree(appData);
        p /= L"ModernSapiAdapter\\Config";
        return p;
    }
    return {};
}

static void ParsePathsString(const std::string& input, std::vector<std::string>& outPaths, std::set<std::string>& seen) {
    size_t pos = 0;
    std::string str = input;
    while ((pos = str.find(',')) != std::string::npos) {
        std::string sub = str.substr(0, pos);
        if (!sub.empty() && !seen.contains(sub)) {
            seen.insert(sub);
            outPaths.push_back(sub);
        }
        str.erase(0, pos + 1);
    }
    if (!str.empty() && !seen.contains(str)) {
        seen.insert(str);
        outPaths.push_back(str);
    }
}

ProviderConfig ConfigParser::LoadMergedConfig(const std::wstring& optionalOutputDir) {
    ProviderConfig config;
    config.EnablePcmCache = true;
    std::set<std::string> seenPaths;
    json mergedJson = json::object();

    std::vector<std::filesystem::path> configLocations;

    std::filesystem::path moduleDir = GetModuleDirectory();
    if (!moduleDir.empty()) {
        configLocations.push_back(moduleDir / L"AzureEmbeddedSpeechProvider_config.json");
    }

    if (!optionalOutputDir.empty() && optionalOutputDir != moduleDir.wstring()) {
        configLocations.push_back(std::filesystem::path(optionalOutputDir) / L"AzureEmbeddedSpeechProvider_config.json");
    }

    std::filesystem::path userDir = GetUserConfigDirectory();
    if (!userDir.empty()) {
        configLocations.push_back(userDir / L"AzureEmbeddedSpeechProvider_config.json");
    }

    for (const auto& path : configLocations) {
        if (std::filesystem::exists(path)) {
            std::ifstream ifs(path);
            if (ifs.is_open()) {
                try {
                    json fileJson = json::parse(ifs);
                    
                    if (fileJson.contains("providerWideConfig") && fileJson["providerWideConfig"].is_object()) {
                        if (!mergedJson.contains("providerWideConfig")) {
                            mergedJson["providerWideConfig"] = json::object();
                        }
                        for (auto& [k, v] : fileJson["providerWideConfig"].items()) {
                            mergedJson["providerWideConfig"][k] = v;
                        }
                    } else if (fileJson.is_object()) {
                        if (!mergedJson.contains("providerWideConfig")) {
                            mergedJson["providerWideConfig"] = json::object();
                        }
                        for (auto& [k, v] : fileJson.items()) {
                            if (k != "voicesConfig") {
                                mergedJson["providerWideConfig"][k] = v;
                            }
                        }
                    }

                    if (fileJson.contains("voicesConfig") && fileJson["voicesConfig"].is_object()) {
                        if (!mergedJson.contains("voicesConfig")) {
                            mergedJson["voicesConfig"] = json::object();
                        }
                        for (auto& [k, v] : fileJson["voicesConfig"].items()) {
                            mergedJson["voicesConfig"][k] = v;
                        }
                    }

                    LogInfo("ConfigParser: Successfully merged config from %ls", path.c_str());
                } catch (const std::exception& e) {
                    LogError("ConfigParser: Failed to parse %ls: %s", path.c_str(), e.what());
                }
            }
        }
    }

    config.FullJson = mergedJson;

    if (mergedJson.contains("providerWideConfig")) {
        auto pwc = mergedJson["providerWideConfig"];

        if (pwc.contains("ExtraVoicePaths") && pwc["ExtraVoicePaths"].is_string()) {
            ParsePathsString(pwc["ExtraVoicePaths"], config.ExtraVoicePaths, seenPaths);
        }

        if (pwc.contains("DecryptionKey") && pwc["DecryptionKey"].is_string()) {
            config.DecryptionKey = pwc["DecryptionKey"];
        }

#ifdef _DEBUG
        config.EnableDebugLogging = true;
#else
        config.EnableDebugLogging = false;
#endif

        if (pwc.contains("EnableDebugLogging")) {
            if (pwc["EnableDebugLogging"].is_boolean()) {
                config.EnableDebugLogging = pwc["EnableDebugLogging"];
            } else if (pwc["EnableDebugLogging"].is_string()) {
                std::string val = pwc["EnableDebugLogging"];
                std::transform(val.begin(), val.end(), val.begin(), ::tolower);
                config.EnableDebugLogging = (val == "true" || val == "1");
            }
        }

        if (pwc.contains("EnablePcmCache")) {
            if (pwc["EnablePcmCache"].is_boolean()) {
                config.EnablePcmCache = pwc["EnablePcmCache"];
            } else if (pwc["EnablePcmCache"].is_string()) {
                std::string val = pwc["EnablePcmCache"];
                std::transform(val.begin(), val.end(), val.begin(), ::tolower);
                config.EnablePcmCache = (val == "true" || val == "1");
            }
        }
    } else {
#ifdef _DEBUG
        config.EnableDebugLogging = true;
#else
        config.EnableDebugLogging = false;
#endif
    }

    return config;
}
