/**
 * @file Logger.h
 * @brief Thread-safe logging facility for the AzureEmbeddedSpeechProvider plugin.
 */

#pragma once

void LogInit();
void LogShutdown();

void LogInfo(const char* fmt, ...);
void LogWarn(const char* fmt, ...);
void LogError(const char* fmt, ...);
