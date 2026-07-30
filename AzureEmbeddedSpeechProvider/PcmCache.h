#pragma once
#include "pch.h"

struct CacheKey {
    std::wstring VoiceName;
    int Rate;
    int Pitch;
    int Volume;
    std::u16string Text;

    bool operator==(const CacheKey& other) const {
        return VoiceName == other.VoiceName &&
               Rate == other.Rate &&
               Pitch == other.Pitch &&
               Volume == other.Volume &&
               Text == other.Text;
    }
};

namespace std {
    template <>
    struct hash<CacheKey> {
        size_t operator()(const CacheKey& k) const {
            size_t res = 17;
            res = res * 31 + hash<std::wstring>()(k.VoiceName);
            res = res * 31 + hash<int>()(k.Rate);
            res = res * 31 + hash<int>()(k.Pitch);
            res = res * 31 + hash<int>()(k.Volume);
            res = res * 31 + hash<std::u16string>()(k.Text);
            return res;
        }
    };
}

struct CachePayload {
    std::vector<uint8_t> AudioData;
    std::vector<ProviderSpeechEvent> Events;
    std::list<std::u16string> StringStore;
};

class PcmCache {
public:
    static bool TryGet(const CacheKey& key, CachePayload& outPayload);
    static void Put(const CacheKey& key, CachePayload&& payload);
    static void LoadFromDisk();
    static void SaveToDisk();

private:
    static constexpr size_t MAX_CACHE_ENTRIES = 4000;
    
    static std::mutex s_mutex;
    static std::list<CacheKey> s_lruList;
    static std::unordered_map<CacheKey, std::pair<CachePayload, std::list<CacheKey>::iterator>> s_cacheMap;
};
