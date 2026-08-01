#include "pch.h"
#include "VoiceManager.h"
#include "ConfigParser.h"
#include "Logger.h"
#include <fstream>
#include <algorithm>

using namespace winrt;
using namespace Windows::Management::Deployment;
using namespace Microsoft::CognitiveServices::Speech;
using json = nlohmann::json;



static std::string GetVoiceAge(const std::string& shortName) {
    std::string lower = shortName;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.find("child") != std::string::npos) return "Child";
    if (lower.find("teen") != std::string::npos) return "Teen";
    if (lower.find("elder") != std::string::npos) return "Elder";
    return "Adult";
}

bool VoiceManager::GenerateVoiceManifest(const std::wstring& outputDir) {
    GlobalLazyInit();
    try {
        json root;
        root["providerName"] = "AzureEmbeddedSpeechProvider";
        root["version"] = "1.0.0";

        json configSchema = json::array();
        
        json decryptionKeyItem;
        decryptionKeyItem["key"] = "DecryptionKey";
        decryptionKeyItem["type"] = "string";
        decryptionKeyItem["displayName"] = "Decryption Key";
        decryptionKeyItem["description"] = "Optional decryption key for voice models.";
        configSchema.push_back(decryptionKeyItem);

        json extraPathsItem;
        extraPathsItem["key"] = "ExtraVoicePaths";
        extraPathsItem["type"] = "string";
        extraPathsItem["displayName"] = "Extra Voice Paths";
        extraPathsItem["description"] = "Comma-separated list of additional directories to scan for offline voice packages.";
        configSchema.push_back(extraPathsItem);

        json enableLoggingItem;
        enableLoggingItem["key"] = "EnableDebugLogging";
        enableLoggingItem["type"] = "boolean";
        enableLoggingItem["displayName"] = "Enable Debug Logging";
        enableLoggingItem["description"] = "If enabled, writes diagnostic traces to the local AppData directory.";
        configSchema.push_back(enableLoggingItem);

        json enablePcmCacheItem;
        enablePcmCacheItem["key"] = "EnablePcmCache";
        enablePcmCacheItem["type"] = "boolean";
        enablePcmCacheItem["displayName"] = "Enable PCM Cache";
        enablePcmCacheItem["description"] = "If enabled, heavily boosts synthesis speeds by bypassing neural generation for repetitive navigation commands.";
        configSchema.push_back(enablePcmCacheItem);

        root["configSchema"] = configSchema;

        std::vector<std::string> paths;

        try {
            PackageManager packageManager;
            auto packages = packageManager.FindPackagesForUser(L"");
            for (auto package : packages) {
                std::wstring pkgName = package.Id().Name().c_str();
                if (pkgName.find(L"MicrosoftWindows.Voice.") == 0) {
                    std::wstring path = package.InstalledLocation().Path().c_str();
                    paths.push_back(WStringToUTF8(path));
                }
            }
        } catch (const winrt::hresult_error& e) {
            LogWarn("VoiceManager: WinRT package enumeration warning: 0x%08X", e.code());
        }

        ProviderConfig providerConfig = ConfigParser::LoadMergedConfig(outputDir);
        for (const auto& p : providerConfig.ExtraVoicePaths) {
            paths.push_back(p);
        }

        json voicesList = json::array();

        if (!paths.empty()) {
            try {
                auto config = EmbeddedSpeechConfig::FromPaths(paths);
                auto synthesizer = SpeechSynthesizer::FromConfig(config, nullptr);
                auto result = synthesizer->GetVoicesAsync().get();

                if (result->Reason == ResultReason::VoicesListRetrieved) {
                    for (const auto& info : result->Voices) {
                        std::string fullName = info->Name;
                        std::string locale = info->Locale;
                        std::string gender = info->Properties.GetProperty("Gender", "Female");

                        std::string shortFriendly = info->ShortName;
                        if (shortFriendly.empty()) {
                            size_t dash = fullName.find(" - ");
                            shortFriendly = (dash != std::string::npos) ? fullName.substr(0, dash) : fullName;
                            if (shortFriendly.rfind("Microsoft ", 0) == 0) {
                                shortFriendly = shortFriendly.substr(10);
                            }
                            size_t paren = shortFriendly.find(" ");
                            if (paren != std::string::npos) {
                                shortFriendly = shortFriendly.substr(0, paren);
                            }
                        }

                        std::string voiceId = fullName;

                        json sapiAttrs;
                        sapiAttrs["Language"] = locale;
                        sapiAttrs["Gender"] = gender.empty() ? "Female" : gender;
                        sapiAttrs["Age"] = GetVoiceAge(shortFriendly);
                        sapiAttrs["Name"] = fullName;
                        sapiAttrs["Vendor"] = "Microsoft";

                        json voiceEntry;
                        voiceEntry["voiceId"] = voiceId;
                        voiceEntry["sapiAttributes"] = sapiAttrs;

                        voicesList.push_back(voiceEntry);
                    }
                } else {
                    LogWarn("VoiceManager: SDK voice query failed: %s", result->ErrorDetails.c_str());
                }
            } catch (const std::exception& e) {
                LogWarn("VoiceManager: Exception querying SDK voices: %s", e.what());
            }
        }

        root["voices"] = voicesList;

        std::filesystem::path outPath = outputDir;
        outPath /= L"AzureEmbeddedSpeechProvider_voices.json";
        
        std::ofstream outFile(outPath);
        if (!outFile.is_open()) {
            LogError("Failed to open %ls for writing manifest", outPath.c_str());
            return false;
        }
        
        outFile << root.dump(4);
        LogInfo("Successfully generated voice manifest at %ls", outPath.c_str());
        return true;
    }
    catch (const winrt::hresult_error& e) {
        LogError("WinRT error generating manifest: 0x%08X", e.code());
        return false;
    }
    catch (const std::exception& e) {
        LogError("Exception generating manifest: %s", e.what());
        return false;
    }
}
