using System;
using System.Globalization;
using Microsoft.Win32;
using SapiManager.Models;

namespace SapiManager.Services;

public static class RegistryManager
{
    public const string CoreEngineClsid = "{9021A4B0-4A3C-4D2A-98C0-84E34F1A5600}"; // Standard CoreEngine COM CLSID
    private const string SapiVoicesKeyPath = @"SOFTWARE\Microsoft\Speech\Voices\Tokens";
    private const string UninstallKeyPath = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\ModernSapiAdapter";

    /// <summary>
    /// Converts a BCP-47 language tag (e.g., "en-US") into a SAPI 5 hex LCID string (e.g., "409").
    /// </summary>
    public static string LanguageTagToHexLcid(string bcp47Tag)
    {
        try
        {
            CultureInfo culture = CultureInfo.GetCultureInfo(bcp47Tag);
            return culture.LCID.ToString("X", CultureInfo.InvariantCulture);
        }
        catch
        {
            return "409"; // Fallback to en-US LCID (409)
        }
    }

    /// <summary>
    /// Writes a SAPI 5 voice token under HKLM\SOFTWARE\Microsoft\Speech\Voices\Tokens.
    /// </summary>
    public static bool RegisterVoiceToken(ProviderManifest manifest, VoiceItem voice, string providerDllPath, string? customAlias = null)
    {
        try
        {
            string tokenKeyName = $"MSA_{manifest.ProviderName}_{voice.VoiceId}";
            using RegistryKey baseKey = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
            using RegistryKey voicesKey = baseKey.CreateSubKey(SapiVoicesKeyPath, true);
            using RegistryKey tokenKey = voicesKey.CreateSubKey(tokenKeyName, true);

            string displayName = !string.IsNullOrWhiteSpace(customAlias) ? customAlias : voice.SapiAttributes.Name;
            tokenKey.SetValue(null, displayName);
            tokenKey.SetValue("CLSID", CoreEngineClsid);
            tokenKey.SetValue("ProviderDll", providerDllPath);
            tokenKey.SetValue("VoiceId", voice.VoiceId);
            tokenKey.SetValue("ProviderName", manifest.ProviderName);

            using RegistryKey attrKey = tokenKey.CreateSubKey("Attributes", true);
            attrKey.SetValue("Language", LanguageTagToHexLcid(voice.SapiAttributes.Language));
            attrKey.SetValue("Gender", voice.SapiAttributes.Gender);
            attrKey.SetValue("Age", voice.SapiAttributes.Age);
            attrKey.SetValue("Name", displayName);
            attrKey.SetValue("Vendor", voice.SapiAttributes.Vendor);

            return true;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"Failed to register voice token {voice.VoiceId}: {ex.Message}");
            return false;
        }
    }

    /// <summary>
    /// Removes a SAPI 5 voice token from registry.
    /// </summary>
    public static bool UnregisterVoiceToken(string providerName, string voiceId)
    {
        try
        {
            string tokenKeyName = $"MSA_{providerName}_{voiceId}";
            using RegistryKey baseKey = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
            using RegistryKey? voicesKey = baseKey.OpenSubKey(SapiVoicesKeyPath, true);
            voicesKey?.DeleteSubKeyTree(tokenKeyName, false);
            return true;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>
    /// Registers the app in Windows Add/Remove Programs registry hive.
    /// </summary>
    public static bool RegisterUninstallInfo(string installDir)
    {
        try
        {
            string exePath = System.IO.Path.Combine(installDir, "SapiManager.exe");
            using RegistryKey baseKey = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
            using RegistryKey uninstallKey = baseKey.CreateSubKey(UninstallKeyPath, true);

            uninstallKey.SetValue("DisplayName", "ModernSapiAdapter SAPI 5 Speech Manager");
            uninstallKey.SetValue("DisplayVersion", "1.0.0");
            uninstallKey.SetValue("Publisher", "ModernSapiAdapter Project");
            uninstallKey.SetValue("UninstallString", $"\"{exePath}\" /uninstall");
            uninstallKey.SetValue("DisplayIcon", exePath);
            uninstallKey.SetValue("InstallLocation", installDir);
            return true;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>
    /// Removes the Add/Remove Programs registry key and all ModernSapiAdapter voice tokens.
    /// </summary>
    public static void RemoveAllAdapterRegistryKeys()
    {
        try
        {
            using RegistryKey baseKey = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
            baseKey.DeleteSubKeyTree(UninstallKeyPath, false);

            using RegistryKey? voicesKey = baseKey.OpenSubKey(SapiVoicesKeyPath, true);
            if (voicesKey != null)
            {
                foreach (string subKeyName in voicesKey.GetSubKeyNames())
                {
                    if (subKeyName.StartsWith("MSA_", StringComparison.OrdinalIgnoreCase))
                    {
                        voicesKey.DeleteSubKeyTree(subKeyName, false);
                    }
                }
            }
        }
        catch
        {
            // Ignore clean-up errors
        }
    }
}
