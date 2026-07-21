using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace SapiManager.Models;

/// <summary>
/// Represents the read-only provider manifest file (&lt;ProviderName&gt;_voices.json).
/// </summary>
public class ProviderManifest
{
    [JsonPropertyName("providerName")]
    public string ProviderName { get; set; } = string.Empty;

    [JsonPropertyName("version")]
    public string Version { get; set; } = "1.0.0";

    [JsonPropertyName("configSchema")]
    public List<ConfigSchemaItem> ConfigSchema { get; set; } = new();

    [JsonPropertyName("voices")]
    public List<VoiceItem> Voices { get; set; } = new();

    [JsonIgnore]
    public string FolderPath { get; set; } = string.Empty;

    [JsonIgnore]
    public string ManifestFilePath { get; set; } = string.Empty;

    [JsonIgnore]
    public string ConfigFilePath { get; set; } = string.Empty;

    [JsonIgnore]
    public string DllFilePath { get; set; } = string.Empty;

    [JsonIgnore]
    public bool IsManifestMissing { get; set; }

    [JsonIgnore]
    public bool IsManifestInvalid { get; set; }
}

public class ConfigSchemaItem
{
    [JsonPropertyName("key")]
    public string Key { get; set; } = string.Empty;

    [JsonPropertyName("type")]
    public string Type { get; set; } = "string"; // "string", "boolean", "securestring"

    [JsonPropertyName("displayName")]
    public string DisplayName { get; set; } = string.Empty;

    [JsonPropertyName("description")]
    public string Description { get; set; } = string.Empty;
}

public class VoiceItem
{
    [JsonPropertyName("voiceId")]
    public string VoiceId { get; set; } = string.Empty;

    [JsonPropertyName("sapiAttributes")]
    public SapiAttributes SapiAttributes { get; set; } = new();
}

public class SapiAttributes
{
    [JsonPropertyName("Language")]
    public string Language { get; set; } = "en-US";

    [JsonPropertyName("Gender")]
    public string Gender { get; set; } = "Neutral";

    [JsonPropertyName("Age")]
    public string Age { get; set; } = "Adult";

    [JsonPropertyName("Name")]
    public string Name { get; set; } = string.Empty;

    [JsonPropertyName("Vendor")]
    public string Vendor { get; set; } = "ModernSapiAdapter";
}

/// <summary>
/// Represents the read/write user configuration file (&lt;ProviderName&gt;_config.json).
/// </summary>
public class ProviderUserConfig
{
    [JsonPropertyName("providerWideConfig")]
    public Dictionary<string, string> ProviderWideConfig { get; set; } = new();

    [JsonPropertyName("voicesConfig")]
    public Dictionary<string, VoiceUserConfigItem> VoicesConfig { get; set; } = new();
}

public class VoiceUserConfigItem
{
    [JsonPropertyName("Enabled")]
    public bool Enabled { get; set; } = true;

    [JsonPropertyName("CustomAlias")]
    public string? CustomAlias { get; set; }
}
