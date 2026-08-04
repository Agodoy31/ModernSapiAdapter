namespace SapiManager.Models;

/// <summary>
/// Result of inspecting a TTS provider package (.zip) before installation.
/// </summary>
public class PackageInspectionResult
{
    public bool IsValid { get; set; }
    public string ErrorMessage { get; set; } = string.Empty;
    public ProviderPackageManifest? Manifest { get; set; }
    public string TargetInstallDir { get; set; } = string.Empty;
    public string? ExistingVersion { get; set; }
    public bool IsRunningProcessDetected { get; set; }
    public int RunningProcessId { get; set; }
}
