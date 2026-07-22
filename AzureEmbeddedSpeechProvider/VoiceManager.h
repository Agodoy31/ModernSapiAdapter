/**
 * @file VoiceManager.h
 * @brief Scans installed MSIX packages and writes the voice manifest.
 */

#pragma once

class VoiceManager {
public:
    static bool GenerateVoiceManifest(const std::wstring& outputDir);
};
