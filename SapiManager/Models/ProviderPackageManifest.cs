using System.Text.Json.Serialization;

namespace SapiManager.Models;

/// <summary>
/// Represents the manifest file (manifest.json) inside a TTS provider package (.zip).
/// </summary>
public class ProviderPackageManifest
{
    [JsonPropertyName("id")]
    public string ProviderId { get; set; } = string.Empty;

    [JsonPropertyName("name")]
    public string ProviderName { get; set; } = string.Empty;

    [JsonPropertyName("version")]
    public string Version { get; set; } = "1.0.0";

    [JsonPropertyName("publisher")]
    public string Publisher { get; set; } = "Unknown";

    [JsonPropertyName("executable")]
    public string ExecutableName { get; set; } = string.Empty;

    [JsonPropertyName("description")]
    public string Description { get; set; } = string.Empty;
}
