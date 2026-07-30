#include "pch.h"
#include "PcmCache.h"
#include "Logger.h"

std::mutex PcmCache::s_mutex;
std::list<CacheKey> PcmCache::s_lruList;
std::unordered_map<CacheKey, std::pair<CachePayload, std::list<CacheKey>::iterator>> PcmCache::s_cacheMap;

bool PcmCache::TryGet(const CacheKey& key, CachePayload& outPayload) {
    std::lock_guard<std::mutex> lock(s_mutex);
    
    auto it = s_cacheMap.find(key);
    if (it == s_cacheMap.end()) {
        return false;
    }
    
    // Move to front of LRU list
    s_lruList.splice(s_lruList.begin(), s_lruList, it->second.second);
    
    // Deep copy the payload to the caller
    outPayload = it->second.first;
    
    // Re-wire the ProviderSpeechEvent pointers to point to the newly copied StringStore
    // because outPayload has its own copy of the StringStore vectors now.
    for (size_t i = 0; i < outPayload.Events.size(); ++i) {
        if (outPayload.Events[i].StringData != nullptr) {
            // Find which string it was pointing to in the original
            const auto& originalEvent = it->second.first.Events[i];
            
            bool rewired = false;
            // Re-point to the matching string in our new StringStore
            for (const auto& str : it->second.first.StringStore) {
                if (originalEvent.StringData == str.c_str()) {
                    // Find the matching string in outPayload's StringStore
                    // They are guaranteed to be in the same order
                    auto outIt = outPayload.StringStore.begin();
                    for (const auto& originalStr : it->second.first.StringStore) {
                        if (&originalStr == &str) {
                            outPayload.Events[i].StringData = outIt->c_str();
                            rewired = true;
                            break;
                        }
                        ++outIt;
                    }
                    break;
                }
            }
            if (!rewired) {
                LogError("PcmCache: CRITICAL ERROR! Failed to rewire StringData pointer for event %zu!", i);
                // Safety fallback to prevent crash:
                outPayload.Events[i].StringData = nullptr;
            }
        }
    }
    
    return true;
}

void PcmCache::Put(const CacheKey& key, CachePayload&& payload) {
    std::lock_guard<std::mutex> lock(s_mutex);
    
    auto it = s_cacheMap.find(key);
    if (it != s_cacheMap.end()) {
        LogInfo("PcmCache: Updating existing entry.");
        s_lruList.splice(s_lruList.begin(), s_lruList, it->second.second);
        it->second.first = std::move(payload);
        return;
    }
    
    // If cache is full, evict the least recently used item (at the back)
    if (s_cacheMap.size() >= MAX_CACHE_ENTRIES) {
        auto last = s_lruList.back();
        LogInfo("PcmCache: Capacity reached (%d). Evicting least recently used entry.", MAX_CACHE_ENTRIES);
        s_cacheMap.erase(last);
        s_lruList.pop_back();
    }
    
    // Insert new item at the front of the LRU list
    s_lruList.push_front(key);
    s_cacheMap.emplace(key, std::make_pair(std::move(payload), s_lruList.begin()));
    
    LogInfo("PcmCache: Inserted new entry. Current size: %zu", s_cacheMap.size());
}

static std::filesystem::path GetCacheFilePath() {
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) {
        std::filesystem::path dir = path;
        CoTaskMemFree(path);
        dir /= L"ModernSapiAdapter";
        dir /= L"Cache";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir / L"AzureSpeechCache.bin";
    }
    return {};
}

void PcmCache::LoadFromDisk() {
    auto path = GetCacheFilePath();
    if (path.empty()) return;

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return;

    uint32_t magic = 0;
    ifs.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (magic != 0xCAFEBABE) {
        LogWarn("PcmCache::LoadFromDisk - Invalid magic number.");
        return;
    }

    uint32_t version = 0;
    ifs.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (version != 2) {
        LogInfo("PcmCache::LoadFromDisk - Old cache version %u detected. Purging cache.", version);
        return;
    }

    uint32_t count = 0;
    ifs.read(reinterpret_cast<char*>(&count), sizeof(count));

    std::lock_guard<std::mutex> lock(s_mutex);
    s_cacheMap.clear();
    s_lruList.clear();

    for (uint32_t i = 0; i < count; ++i) {
        CacheKey key;
        uint32_t vnLen = 0;
        ifs.read(reinterpret_cast<char*>(&vnLen), sizeof(vnLen));
        if (vnLen > 0) {
            key.VoiceName.resize(vnLen);
            ifs.read(reinterpret_cast<char*>(&key.VoiceName[0]), vnLen * sizeof(wchar_t));
        }

        ifs.read(reinterpret_cast<char*>(&key.Rate), sizeof(key.Rate));
        ifs.read(reinterpret_cast<char*>(&key.Pitch), sizeof(key.Pitch));
        ifs.read(reinterpret_cast<char*>(&key.Volume), sizeof(key.Volume));

        uint32_t txtLen = 0;
        ifs.read(reinterpret_cast<char*>(&txtLen), sizeof(txtLen));
        if (txtLen > 0) {
            key.Text.resize(txtLen);
            ifs.read(reinterpret_cast<char*>(&key.Text[0]), txtLen * sizeof(char16_t));
        }

        CachePayload payload;
        uint32_t audioSize = 0;
        ifs.read(reinterpret_cast<char*>(&audioSize), sizeof(audioSize));
        if (audioSize > 0) {
            payload.AudioData.resize(audioSize);
            ifs.read(reinterpret_cast<char*>(payload.AudioData.data()), audioSize);
        }

        uint32_t evCount = 0;
        ifs.read(reinterpret_cast<char*>(&evCount), sizeof(evCount));
        for (uint32_t j = 0; j < evCount; ++j) {
            ProviderSpeechEvent ev = {};
            ifs.read(reinterpret_cast<char*>(&ev.EventType), sizeof(ev.EventType));
            ifs.read(reinterpret_cast<char*>(&ev.TextOffset), sizeof(ev.TextOffset));
            ifs.read(reinterpret_cast<char*>(&ev.TextLength), sizeof(ev.TextLength));
            ifs.read(reinterpret_cast<char*>(&ev.AudioByteOffset), sizeof(ev.AudioByteOffset));

            uint32_t strLen = 0;
            ifs.read(reinterpret_cast<char*>(&strLen), sizeof(strLen));
            if (strLen > 0) {
                std::u16string strData;
                strData.resize(strLen);
                ifs.read(reinterpret_cast<char*>(&strData[0]), strLen * sizeof(char16_t));
                payload.StringStore.push_back(strData);
                ev.StringData = payload.StringStore.back().c_str();
            } else {
                ev.StringData = nullptr;
            }
            payload.Events.push_back(ev);
        }

        s_lruList.push_back(key);
        s_cacheMap.emplace(key, std::make_pair(std::move(payload), --s_lruList.end()));
    }

    LogInfo("PcmCache: Loaded %zu entries from disk.", s_cacheMap.size());
}

void PcmCache::SaveToDisk() {
    auto path = GetCacheFilePath();
    if (path.empty()) return;

    auto tmpPath = path;
    tmpPath.replace_extension(L".tmp");

    std::ofstream ofs(tmpPath, std::ios::binary);
    if (!ofs.is_open()) return;

    uint32_t magic = 0xCAFEBABE;
    ofs.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    uint32_t version = 2;
    ofs.write(reinterpret_cast<const char*>(&version), sizeof(version));

    std::lock_guard<std::mutex> lock(s_mutex);
    uint32_t count = static_cast<uint32_t>(s_cacheMap.size());
    ofs.write(reinterpret_cast<const char*>(&count), sizeof(count));

    for (const auto& key : s_lruList) {
        auto it = s_cacheMap.find(key);
        if (it == s_cacheMap.end()) continue;

        uint32_t vnLen = static_cast<uint32_t>(key.VoiceName.length());
        ofs.write(reinterpret_cast<const char*>(&vnLen), sizeof(vnLen));
        if (vnLen > 0) {
            ofs.write(reinterpret_cast<const char*>(key.VoiceName.data()), vnLen * sizeof(wchar_t));
        }

        ofs.write(reinterpret_cast<const char*>(&key.Rate), sizeof(key.Rate));
        ofs.write(reinterpret_cast<const char*>(&key.Pitch), sizeof(key.Pitch));
        ofs.write(reinterpret_cast<const char*>(&key.Volume), sizeof(key.Volume));

        uint32_t txtLen = static_cast<uint32_t>(key.Text.length());
        ofs.write(reinterpret_cast<const char*>(&txtLen), sizeof(txtLen));
        if (txtLen > 0) {
            ofs.write(reinterpret_cast<const char*>(key.Text.data()), txtLen * sizeof(char16_t));
        }

        const auto& payload = it->second.first;
        uint32_t audioSize = static_cast<uint32_t>(payload.AudioData.size());
        ofs.write(reinterpret_cast<const char*>(&audioSize), sizeof(audioSize));
        if (audioSize > 0) {
            ofs.write(reinterpret_cast<const char*>(payload.AudioData.data()), audioSize);
        }

        uint32_t evCount = static_cast<uint32_t>(payload.Events.size());
        ofs.write(reinterpret_cast<const char*>(&evCount), sizeof(evCount));
        for (const auto& ev : payload.Events) {
            ofs.write(reinterpret_cast<const char*>(&ev.EventType), sizeof(ev.EventType));
            ofs.write(reinterpret_cast<const char*>(&ev.TextOffset), sizeof(ev.TextOffset));
            ofs.write(reinterpret_cast<const char*>(&ev.TextLength), sizeof(ev.TextLength));
            ofs.write(reinterpret_cast<const char*>(&ev.AudioByteOffset), sizeof(ev.AudioByteOffset));

            uint32_t strLen = 0;
            if (ev.StringData != nullptr) {
                strLen = static_cast<uint32_t>(std::char_traits<char16_t>::length(ev.StringData));
            }
            ofs.write(reinterpret_cast<const char*>(&strLen), sizeof(strLen));
            if (strLen > 0) {
                ofs.write(reinterpret_cast<const char*>(ev.StringData), strLen * sizeof(char16_t));
            }
        }
    }
    
    ofs.close();

    std::error_code ec;
    std::filesystem::rename(tmpPath, path, ec);
    if (!ec) {
        LogInfo("PcmCache: Saved %u entries to disk.", count);
    } else {
        LogError("PcmCache: Failed to rename tmp file. Error: %s", ec.message().c_str());
    }
}
