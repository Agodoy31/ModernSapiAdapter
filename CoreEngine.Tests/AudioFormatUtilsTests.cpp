#include "pch.h"
#include "../CoreEngine/AudioFormatUtils.h"

TEST(AudioFormatUtilsTests, TryParseAudioFormatJsonParsesValidFormat) {
    nlohmann::json formatJson = {
        {"type", "raw"},
        {"container", "raw"},
        {"encoding", "pcm"},
        {"sample_rate", 22050},
        {"bits_per_sample", 16},
        {"channels", 1}
    };

    WAVEFORMATEX format = {};
    EXPECT_TRUE(AudioFormatUtils::TryParseAudioFormatJson(formatJson, format));
    EXPECT_EQ(format.wFormatTag, WAVE_FORMAT_PCM);
    EXPECT_EQ(format.nSamplesPerSec, 22050u);
    EXPECT_EQ(format.wBitsPerSample, 16u);
    EXPECT_EQ(format.nChannels, 1u);
    EXPECT_EQ(format.nBlockAlign, 2u);
    EXPECT_EQ(format.nAvgBytesPerSec, 44100u);
    EXPECT_EQ(format.cbSize, 0u);
}

TEST(AudioFormatUtilsTests, TryParseAudioFormatJsonRejectsInvalidFormats) {
    WAVEFORMATEX format = {};

    EXPECT_FALSE(AudioFormatUtils::TryParseAudioFormatJson(nlohmann::json(nullptr), format));
    EXPECT_FALSE(AudioFormatUtils::TryParseAudioFormatJson(nlohmann::json::array(), format));

    // Missing sample_rate
    EXPECT_FALSE(AudioFormatUtils::TryParseAudioFormatJson(
        nlohmann::json{{"bits_per_sample", 16}, {"channels", 1}}, format));

    // Zero sample_rate
    EXPECT_FALSE(AudioFormatUtils::TryParseAudioFormatJson(
        nlohmann::json{{"sample_rate", 0}, {"bits_per_sample", 16}, {"channels", 1}}, format));

    // Invalid block alignment (not byte multiple)
    EXPECT_FALSE(AudioFormatUtils::TryParseAudioFormatJson(
        nlohmann::json{{"sample_rate", 22050}, {"bits_per_sample", 7}, {"channels", 1}}, format));
}

TEST(AudioFormatUtilsTests, WaveFormatExToJsonSerializesCorrectly) {
    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nSamplesPerSec = 44100;
    format.wBitsPerSample = 16;
    format.nChannels = 2;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = 176400;

    nlohmann::json json = AudioFormatUtils::WaveFormatExToJson(format);
    EXPECT_EQ(json["sample_rate"], 44100u);
    EXPECT_EQ(json["bits_per_sample"], 16u);
    EXPECT_EQ(json["channels"], 2u);
    EXPECT_EQ(json["encoding"], "pcm");
}
