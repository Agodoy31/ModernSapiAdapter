using System;
using System.Globalization;
using Microsoft.Win32;
using SapiManager.Models;

namespace SapiManager.Services;

public static class RegistryManager
{
    // IMPORTANT: This CLSID is strictly coupled with the CoreEngine unmanaged COM DLL.
    // If you change this, you MUST also update CLSID_SapiEngine in CoreEngine/dllmain.cpp
    public const string CoreEngineClsid = "{91CD243C-63F7-441F-AE2F-45057005CB6D}"; // Standard CoreEngine COM CLSID
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
    public static bool RegisterVoiceToken(
        string voiceTokenName,
        string voiceName,
        string voiceId,
        string providerExePath,
        string providerPipeName,
        string bcp47Language,
        string gender,
        string vendor)
    {
        try
        {
            using RegistryKey baseKey = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
            using RegistryKey voicesKey = baseKey.CreateSubKey(SapiVoicesKeyPath, true);
            using RegistryKey tokenKey = voicesKey.CreateSubKey(voiceTokenName, true);

            tokenKey.SetValue(null, voiceName);
            tokenKey.SetValue("CLSID", CoreEngineClsid);
            tokenKey.SetValue("ProviderExecutablePath", providerExePath);
            tokenKey.SetValue("ProviderPipeName", providerPipeName);
            tokenKey.SetValue("VoiceId", voiceId);

            using RegistryKey attrKey = tokenKey.CreateSubKey("Attributes", true);
            attrKey.SetValue("Name", voiceName);
            attrKey.SetValue("Language", LanguageTagToHexLcid(bcp47Language));
            attrKey.SetValue("Gender", gender ?? "Neutral");
            attrKey.SetValue("Age", "Adult");
            attrKey.SetValue("Vendor", vendor ?? "Unknown");

            return true;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"Failed to register voice token {voiceTokenName}: {ex.Message}");
            return false;
        }
    }

    /// <summary>
    /// Legacy overload for ProviderManifest registration.
    /// </summary>
    public static bool RegisterVoiceToken(ProviderManifest manifest, VoiceItem voice, string providerDllPath, string? customAlias = null)
    {
        string tokenKeyName = $"MSA_{manifest.ProviderName}_{voice.VoiceId}";
        string displayName = !string.IsNullOrWhiteSpace(customAlias) ? customAlias : voice.SapiAttributes.Name;
        return RegisterVoiceToken(
            voiceTokenName: tokenKeyName,
            voiceName: displayName,
            voiceId: voice.VoiceId,
            providerExePath: providerDllPath,
            providerPipeName: manifest.ProviderName,
            bcp47Language: voice.SapiAttributes.Language,
            gender: voice.SapiAttributes.Gender,
            vendor: voice.SapiAttributes.Vendor);
    }

    /// <summary>
    /// Removes a SAPI 5 voice token from registry by token key name.
    /// </summary>
    public static bool UnregisterVoiceToken(string voiceTokenName)
    {
        try
        {
            using RegistryKey baseKey = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
            using RegistryKey? voicesKey = baseKey.OpenSubKey(SapiVoicesKeyPath, true);
            voicesKey?.DeleteSubKeyTree(voiceTokenName, false);
            return true;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"Failed to unregister voice token {voiceTokenName}: {ex.Message}");
            return false;
        }
    }

    /// <summary>
    /// Legacy overload for providerName + voiceId unregistration.
    /// </summary>
    public static bool UnregisterVoiceToken(string providerName, string voiceId)
    {
        string tokenKeyName = $"MSA_{providerName}_{voiceId}";
        return UnregisterVoiceToken(tokenKeyName);
    }

    /// <summary>
    /// Checks if a SAPI 5 voice token key exists in HKLM\SOFTWARE\Microsoft\Speech\Voices\Tokens.
    /// </summary>
    public static bool IsVoiceTokenRegistered(string voiceTokenName)
    {
        try
        {
            using RegistryKey baseKey = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
            using RegistryKey? voicesKey = baseKey.OpenSubKey(SapiVoicesKeyPath, false);
            if (voicesKey == null) return false;

            using RegistryKey? tokenKey = voicesKey.OpenSubKey(voiceTokenName, false);
            return tokenKey != null;
        }
        catch
        {
            return false;
        }
    }


    /// <summary>
    /// Gets the installation location from the registry, if present.
    /// </summary>
    public static string? GetInstallLocation()
    {
        try
        {
            using RegistryKey baseKey = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
            using RegistryKey? uninstallKey = baseKey.OpenSubKey(UninstallKeyPath, false);
            return uninstallKey?.GetValue("InstallLocation") as string;
        }
        catch
        {
            return null;
        }
    }

    /// <summary>
    /// Gets the installed display version from the registry, if present.
    /// </summary>
    public static string? GetInstalledVersion()
    {
        try
        {
            using RegistryKey baseKey = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
            using RegistryKey? uninstallKey = baseKey.OpenSubKey(UninstallKeyPath, false);
            return uninstallKey?.GetValue("DisplayVersion") as string;
        }
        catch
        {
            return null;
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
            string version = System.Reflection.Assembly.GetExecutingAssembly().GetName().Version?.ToString() ?? "1.0.0";
            
            using RegistryKey baseKey = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
            using RegistryKey uninstallKey = baseKey.CreateSubKey(UninstallKeyPath, true);

            uninstallKey.SetValue("DisplayName", "ModernSapiAdapter SAPI 5 Speech Manager");
            uninstallKey.SetValue("DisplayVersion", version);
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
