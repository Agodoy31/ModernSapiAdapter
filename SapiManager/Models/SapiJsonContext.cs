using System.Text.Json.Serialization;

namespace SapiManager.Models;

[JsonSourceGenerationOptions(
    WriteIndented = false,
    PropertyNameCaseInsensitive = true,
    DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull)]
[JsonSerializable(typeof(InfoRequest))]
[JsonSerializable(typeof(InfoResponse))]
[JsonSerializable(typeof(VoicesRequest))]
[JsonSerializable(typeof(VoicesResponse))]
[JsonSerializable(typeof(ShutdownRequest))]
[JsonSerializable(typeof(AudioFormatInfo))]
[JsonSerializable(typeof(VoiceInfo))]
[JsonSerializable(typeof(ProviderPackageManifest))]
[JsonSerializable(typeof(ProviderManifest))]
[JsonSerializable(typeof(ProviderUserConfig))]
public partial class SapiJsonContext : JsonSerializerContext
{
}
