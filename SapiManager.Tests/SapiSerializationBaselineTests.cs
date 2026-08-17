using System.Text.Json;
using SapiManager.Models;
using Xunit;

namespace SapiManager.Tests;

public class SapiSerializationBaselineTests
{
    [Fact]
    public void InfoRequest_Serialization_MatchesExpectedFormat()
    {
        var request = new InfoRequest();
        string json = JsonSerializer.Serialize(request, SapiJsonContext.Default.InfoRequest);
        Assert.Contains("\"command\":\"info\"", json);
    }

    [Fact]
    public void VoicesRequest_Serialization_MatchesExpectedFormat()
    {
        var request = new VoicesRequest();
        string json = JsonSerializer.Serialize(request, SapiJsonContext.Default.VoicesRequest);
        Assert.Contains("\"command\":\"voices\"", json);
    }

    [Fact]
    public void ShutdownRequest_Serialization_MatchesExpectedFormat()
    {
        var request = new ShutdownRequest();
        string json = JsonSerializer.Serialize(request, SapiJsonContext.Default.ShutdownRequest);
        Assert.Contains("\"command\":\"shutdown\"", json);
    }

    [Fact]
    public void InfoResponse_Deserialization_HydratesPropertiesCorrectly()
    {
        string json = "{\"response\":\"info\",\"provider_name\":\"TestProvider\",\"version\":\"1.0.0\",\"supports_ssml\":true,\"audio_format\":{\"sample_rate\":24000,\"bits_per_sample\":16,\"channels\":1}}";
        var response = JsonSerializer.Deserialize(json, SapiJsonContext.Default.InfoResponse);
        Assert.NotNull(response);
        Assert.Equal("info", response.Response);
        Assert.Equal("TestProvider", response.ProviderName);
        Assert.Equal("1.0.0", response.Version);
        Assert.True(response.SupportsSsml);
        Assert.NotNull(response.AudioFormat);
        Assert.Equal(24000, response.AudioFormat.SampleRate);
        Assert.Equal(16, response.AudioFormat.BitsPerSample);
        Assert.Equal(1, response.AudioFormat.Channels);
    }

    [Fact]
    public void VoicesResponse_Deserialization_HydratesPropertiesCorrectly()
    {
        string json = "{\"response\":\"voices\",\"voices\":[{\"id\":\"v1\",\"name\":\"Voice 1\",\"language\":\"en-US\",\"gender\":\"Female\",\"vendor\":\"Vendor1\"}]}";
        var response = JsonSerializer.Deserialize(json, SapiJsonContext.Default.VoicesResponse);
        Assert.NotNull(response);
        Assert.Equal("voices", response.Response);
        Assert.Single(response.Voices);
        Assert.Equal("v1", response.Voices[0].Id);
        Assert.Equal("Voice 1", response.Voices[0].Name);
        Assert.Equal("en-US", response.Voices[0].Language);
        Assert.Equal("Female", response.Voices[0].Gender);
        Assert.Equal("Vendor1", response.Voices[0].Vendor);
    }

    [Fact]
    public void ProviderPackageManifest_Deserialization_HydratesPropertiesCorrectly()
    {
        string json = "{\"id\":\"test-id\",\"name\":\"Test Provider\",\"version\":\"1.2.3\",\"publisher\":\"Test Corp\",\"executable\":\"bin/test.exe\",\"description\":\"Test package\"}";
        var manifest = JsonSerializer.Deserialize(json, SapiJsonContext.Default.ProviderPackageManifest);
        Assert.NotNull(manifest);
        Assert.Equal("test-id", manifest.ProviderId);
        Assert.Equal("Test Provider", manifest.ProviderName);
        Assert.Equal("1.2.3", manifest.Version);
        Assert.Equal("Test Corp", manifest.Publisher);
        Assert.Equal("bin/test.exe", manifest.ExecutableName);
        Assert.Equal("Test package", manifest.Description);
    }

    [Fact]
    public void ProviderManifest_Deserialization_HydratesPropertiesCorrectly()
    {
        string json = "{\"providerName\":\"TestEngine\",\"version\":\"2.0.0\",\"configSchema\":[{\"key\":\"ApiKey\",\"type\":\"securestring\",\"displayName\":\"API Key\",\"description\":\"Key for API\"}],\"voices\":[{\"voiceId\":\"v1\",\"sapiAttributes\":{\"Language\":\"en-US\",\"Gender\":\"Neutral\",\"Age\":\"Adult\",\"Name\":\"Test Voice\",\"Vendor\":\"ModernSapiAdapter\"}}]}";
        var manifest = JsonSerializer.Deserialize(json, SapiJsonContext.Default.ProviderManifest);
        Assert.NotNull(manifest);
        Assert.Equal("TestEngine", manifest.ProviderName);
        Assert.Equal("2.0.0", manifest.Version);
        Assert.Single(manifest.ConfigSchema);
        Assert.Equal("ApiKey", manifest.ConfigSchema[0].Key);
        Assert.Single(manifest.Voices);
        Assert.Equal("v1", manifest.Voices[0].VoiceId);
        Assert.Equal("Test Voice", manifest.Voices[0].SapiAttributes.Name);
    }

    [Fact]
    public void ProviderUserConfig_Deserialization_HydratesPropertiesCorrectly()
    {
        string json = "{\"providerWideConfig\":{\"LogLevel\":\"Debug\"},\"voicesConfig\":{\"v1\":{\"Enabled\":true,\"CustomAlias\":\"My Voice\"}}}";
        var config = JsonSerializer.Deserialize(json, SapiJsonContext.Default.ProviderUserConfig);
        Assert.NotNull(config);
        Assert.True(config.ProviderWideConfig.ContainsKey("LogLevel"));
        Assert.Equal("Debug", config.ProviderWideConfig["LogLevel"]);
        Assert.True(config.VoicesConfig.ContainsKey("v1"));
        Assert.True(config.VoicesConfig["v1"].Enabled);
        Assert.Equal("My Voice", config.VoicesConfig["v1"].CustomAlias);
    }

    [Fact]
    public void RegistryManager_CurrentVersion_ReturnsValidVersion()
    {
        var version = SapiManager.Services.RegistryManager.CurrentVersion;
        Assert.NotNull(version);
        Assert.True(version.Major >= 2);
    }

    [Theory]
    [InlineData("en-US", "409")]
    [InlineData("es-ES", "C0A")]
    [InlineData("fr-FR", "40C")]
    [InlineData("ja-JP", "411")]
    [InlineData("", "409")]
    [InlineData("invalid-tag-xyz", "409")]
    public void LanguageTagToHexLcid_ReturnsCorrectHex(string tag, string expectedHex)
    {
        string lcid = SapiManager.Services.RegistryManager.LanguageTagToHexLcid(tag);
        Assert.Equal(expectedHex, lcid, ignoreCase: true);
    }

    [Fact]
    public void VoiceViewModel_AccessibleDescription_ReturnsExpectedFormat()
    {
        var vm = new VoiceViewModel
        {
            Name = "Jenny",
            Language = "en-US",
            Gender = "Female",
            Vendor = "Microsoft"
        };

        Assert.Equal("Jenny, en-US, Female, Microsoft", vm.AccessibleDescription);
    }

    [Fact]
    public void VoiceViewModel_PropertyChanged_FiresForAccessibleDescription()
    {
        var vm = new VoiceViewModel();
        var changedProps = new List<string?>();
        vm.PropertyChanged += (s, e) => changedProps.Add(e.PropertyName);

        vm.Name = "David";

        Assert.Contains(nameof(VoiceViewModel.AccessibleDescription), changedProps);
    }
}
