/**
 * @file AzureEmbeddedSynthesizer.h
 * @brief Core speech synthesis dispatcher for AzureEmbeddedSpeechProvider.
 */

#pragma once

#include "provider_abi.h"

class AzureEmbeddedSynthesizer {
public:
    static bool Speak(const ProviderSpeakParams* params);
};
