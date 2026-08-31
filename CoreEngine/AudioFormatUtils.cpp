#include "pch.h"
#include "AudioFormatUtils.h"
#include "JsonValue.h"
#include <limits>

namespace AudioFormatUtils
{

bool TryParseAudioFormatJson(
    const nlohmann::json& formatJson,
    WAVEFORMATEX& format) noexcept
{
    if (!formatJson.is_object())
    {
        return false;
    }

    auto isPositiveInteger = [&](const char* name, uint64_t maximum, uint64_t& value) noexcept {
        if (!formatJson.contains(name))
        {
            return false;
        }
        if (!TryGetJsonUnsignedInteger(formatJson[name], value))
        {
            return false;
        }
        if (value == 0 || value > maximum)
        {
            return false;
        }
        return true;
    };

    uint64_t sampleRate = 0;
    uint64_t bitsPerSample = 0;
    uint64_t channels = 0;
    if (!isPositiveInteger("sample_rate", (std::numeric_limits<DWORD>::max)(), sampleRate) ||
        !isPositiveInteger("bits_per_sample", (std::numeric_limits<WORD>::max)(), bitsPerSample) ||
        !isPositiveInteger("channels", (std::numeric_limits<WORD>::max)(), channels))
    {
        return false;
    }

    const uint64_t blockAlignment = (channels * bitsPerSample) / 8;
    if (blockAlignment == 0 || blockAlignment > (std::numeric_limits<WORD>::max)() ||
        channels * bitsPerSample != blockAlignment * 8 ||
        sampleRate > (std::numeric_limits<DWORD>::max)() / blockAlignment)
    {
        return false;
    }

    format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nSamplesPerSec = static_cast<DWORD>(sampleRate);
    format.wBitsPerSample = static_cast<WORD>(bitsPerSample);
    format.nChannels = static_cast<WORD>(channels);
    format.nBlockAlign = static_cast<WORD>(blockAlignment);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    format.cbSize = 0;
    return true;
}

nlohmann::json WaveFormatExToJson(
    const WAVEFORMATEX& format)
{
    return nlohmann::json{
        {"type", "raw"},
        {"container", "raw"},
        {"encoding", "pcm"},
        {"sample_rate", format.nSamplesPerSec},
        {"bits_per_sample", format.wBitsPerSample},
        {"channels", format.nChannels}
    };
}

} // namespace AudioFormatUtils
