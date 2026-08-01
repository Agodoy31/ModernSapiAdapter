#pragma once
#include "pch.h"

struct ProviderConfig {
    std::vector<std::string> ExtraVoicePaths;
    std::string DecryptionKey;
    bool EnableDebugLogging;
    bool EnablePcmCache;
    nlohmann::json FullJson;
};

class ConfigParser {
public:
    /**
     * @brief Loads and merges Machine-Wide config (alongside DLL / ProgramData) 
     *        with User-Wide config (%LOCALAPPDATA%). User settings override Machine settings.
     * @param optionalOutputDir Optional directory override (e.g. passed during manifest generation)
     */
    static ProviderConfig LoadMergedConfig(const std::wstring& optionalOutputDir = L"");

private:
    static std::filesystem::path GetModuleDirectory();
    static std::filesystem::path GetUserConfigDirectory();
};
