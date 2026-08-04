using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace SapiManager.Models
{
    public class InfoRequest
    {
        [JsonPropertyName("command")]
        public string Command { get; set; } = "info";
    }

    public class VoicesRequest
    {
        [JsonPropertyName("command")]
        public string Command { get; set; } = "voices";
    }

    public class ShutdownRequest
    {
        [JsonPropertyName("command")]
        public string Command { get; set; } = "shutdown";
    }

    public class AudioFormatInfo
    {
        [JsonPropertyName("sample_rate")]
        public int SampleRate { get; set; }

        [JsonPropertyName("bits_per_sample")]
        public int BitsPerSample { get; set; }

        [JsonPropertyName("channels")]
        public int Channels { get; set; }
    }

    public class InfoResponse
    {
        [JsonPropertyName("response")]
        public string? Response { get; set; }

        [JsonPropertyName("provider_name")]
        public string? ProviderName { get; set; }

        [JsonPropertyName("version")]
        public string? Version { get; set; }

        [JsonPropertyName("supports_ssml")]
        public bool SupportsSsml { get; set; }

        [JsonPropertyName("audio_format")]
        public AudioFormatInfo? AudioFormat { get; set; }
    }

    public class VoiceInfo
    {
        [JsonPropertyName("id")]
        public string Id { get; set; } = string.Empty;

        [JsonPropertyName("name")]
        public string Name { get; set; } = string.Empty;

        [JsonPropertyName("language")]
        public string Language { get; set; } = string.Empty;

        [JsonPropertyName("gender")]
        public string Gender { get; set; } = string.Empty;

        [JsonPropertyName("vendor")]
        public string Vendor { get; set; } = string.Empty;
    }

    public class VoicesResponse
    {
        [JsonPropertyName("response")]
        public string? Response { get; set; }

        [JsonPropertyName("voices")]
        public List<VoiceInfo> Voices { get; set; } = new();
    }

    public class ProviderProbeResult
    {
        public string ProviderId { get; set; } = string.Empty;
        public string ExePath { get; set; } = string.Empty;
        public InfoResponse? Info { get; set; }
        public List<VoiceInfo> Voices { get; set; } = new();
    }
}
