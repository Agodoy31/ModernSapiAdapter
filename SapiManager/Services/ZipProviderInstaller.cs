using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text.Json;
using SapiManager.Models;

namespace SapiManager.Services;

/// <summary>
/// Service responsible for inspecting, terminating, and extracting ZIP-based provider packages.
/// </summary>
public class ZipProviderInstaller
{
    /// <summary>
    /// Inspects a provider ZIP package to validate its structure, extract or infer its manifest,
    /// determine target installation path, check for existing version, and detect running processes.
    /// </summary>
    /// <param name="zipPath">Path to the provider ZIP package.</param>
    /// <param name="baseProvidersDir">Base directory where providers are installed.</param>
    /// <returns>A PackageInspectionResult containing package info and system status.</returns>
    public PackageInspectionResult InspectPackage(string zipPath, string baseProvidersDir)
    {
        if (string.IsNullOrWhiteSpace(zipPath) || !File.Exists(zipPath))
        {
            return new PackageInspectionResult
            {
                IsValid = false,
                ErrorMessage = $"Zip package not found: '{zipPath}'"
            };
        }

        ProviderPackageManifest? manifest = null;

        try
        {
            using var archive = ZipFile.OpenRead(zipPath);

            // 1. Look for manifest.json
            var manifestEntry = archive.Entries.FirstOrDefault(e =>
                e.FullName.Equals("manifest.json", StringComparison.OrdinalIgnoreCase) ||
                e.Name.Equals("manifest.json", StringComparison.OrdinalIgnoreCase));

            if (manifestEntry != null)
            {
                using var stream = manifestEntry.Open();
                var jsonOptions = new JsonSerializerOptions { PropertyNameCaseInsensitive = true };
                manifest = JsonSerializer.Deserialize<ProviderPackageManifest>(stream, jsonOptions);
            }

            // 2. Fallback to PE FileVersionInfo header reading on the first .exe if manifest.json is absent
            if (manifest == null || string.IsNullOrWhiteSpace(manifest.ProviderId))
            {
                var exeEntry = archive.Entries.FirstOrDefault(e =>
                    e.FullName.EndsWith(".exe", StringComparison.OrdinalIgnoreCase));

                if (exeEntry == null)
                {
                    return new PackageInspectionResult
                    {
                        IsValid = false,
                        ErrorMessage = "No manifest.json or executable (.exe) found in package."
                    };
                }

                string tempDir = Path.Combine(Path.GetTempPath(), "ModernSapiAdapter_ZipInspect_" + Guid.NewGuid().ToString("N"));
                try
                {
                    Directory.CreateDirectory(tempDir);
                    string tempExePath = Path.Combine(tempDir, Path.GetFileName(exeEntry.FullName));
                    exeEntry.ExtractToFile(tempExePath, overwrite: true);

                    FileVersionInfo info = FileVersionInfo.GetVersionInfo(tempExePath);
                    string exeBaseName = Path.GetFileNameWithoutExtension(exeEntry.FullName);

                    manifest = new ProviderPackageManifest
                    {
                        ProviderId = exeBaseName,
                        ProviderName = !string.IsNullOrWhiteSpace(info.ProductName) ? info.ProductName : exeBaseName,
                        Version = !string.IsNullOrWhiteSpace(info.FileVersion) ? info.FileVersion : (!string.IsNullOrWhiteSpace(info.ProductVersion) ? info.ProductVersion : "1.0.0"),
                        Publisher = !string.IsNullOrWhiteSpace(info.CompanyName) ? info.CompanyName : "Unknown",
                        ExecutableName = exeEntry.FullName,
                        Description = !string.IsNullOrWhiteSpace(info.FileDescription) ? info.FileDescription : string.Empty
                    };
                }
                finally
                {
                    if (Directory.Exists(tempDir))
                    {
                        try { Directory.Delete(tempDir, recursive: true); } catch { }
                    }
                }
            }
        }
        catch (Exception ex)
        {
            return new PackageInspectionResult
            {
                IsValid = false,
                ErrorMessage = $"Failed to inspect package: {ex.Message}"
            };
        }

        if (manifest == null || string.IsNullOrWhiteSpace(manifest.ProviderId))
        {
            return new PackageInspectionResult
            {
                IsValid = false,
                ErrorMessage = "Invalid provider manifest or could not determine provider ID."
            };
        }

        string targetInstallDir = Path.Combine(baseProvidersDir, manifest.ProviderId);
        string? existingVersion = null;

        // Check if target install dir already exists and read version of installed executable if present
        if (Directory.Exists(targetInstallDir) && !string.IsNullOrWhiteSpace(manifest.ExecutableName))
        {
            string installedExePath = Path.Combine(targetInstallDir, manifest.ExecutableName);
            if (File.Exists(installedExePath))
            {
                try
                {
                    FileVersionInfo installedInfo = FileVersionInfo.GetVersionInfo(installedExePath);
                    existingVersion = !string.IsNullOrWhiteSpace(installedInfo.FileVersion)
                        ? installedInfo.FileVersion
                        : (!string.IsNullOrWhiteSpace(installedInfo.ProductVersion) ? installedInfo.ProductVersion : "Unknown");
                }
                catch
                {
                    existingVersion = "Unknown";
                }
            }
        }

        // Check if provider executable process is currently running
        bool isRunning = false;
        int runningProcessId = 0;

        if (!string.IsNullOrWhiteSpace(manifest.ExecutableName))
        {
            string processName = Path.GetFileNameWithoutExtension(manifest.ExecutableName);
            var processes = Process.GetProcessesByName(processName);
            if (processes.Length > 0)
            {
                isRunning = true;
                runningProcessId = processes[0].Id;
            }
        }

        return new PackageInspectionResult
        {
            IsValid = true,
            ErrorMessage = string.Empty,
            Manifest = manifest,
            TargetInstallDir = targetInstallDir,
            ExistingVersion = existingVersion,
            IsRunningProcessDetected = isRunning,
            RunningProcessId = runningProcessId
        };
    }

    /// <summary>
    /// Terminates a process by its ID, including its entire process tree.
    /// </summary>
    /// <param name="processId">The ID of the process to terminate.</param>
    /// <returns>True if process was successfully terminated or not running; otherwise false.</returns>
    public bool TerminateProcess(int processId)
    {
        if (processId <= 0)
        {
            return false;
        }

        try
        {
            using var proc = Process.GetProcessById(processId);
            if (proc.HasExited)
            {
                return true;
            }

            proc.Kill(entireProcessTree: true);
            proc.WaitForExit(3000);
            return proc.HasExited;
        }
        catch (ArgumentException)
        {
            // Process is no longer running
            return true;
        }
        catch (Exception)
        {
            return false;
        }
    }

    /// <summary>
    /// Extracts the ZIP package into the specified target installation directory.
    /// Creates the target directory if missing and overwrites existing files.
    /// </summary>
    /// <param name="zipPath">Path to the ZIP package.</param>
    /// <param name="targetInstallDir">Path to the target installation directory.</param>
    /// <returns>True if extraction succeeded; false otherwise.</returns>
    public bool ExtractAndInstall(string zipPath, string targetInstallDir)
    {
        if (string.IsNullOrWhiteSpace(zipPath) || !File.Exists(zipPath))
        {
            return false;
        }

        try
        {
            if (!Directory.Exists(targetInstallDir))
            {
                Directory.CreateDirectory(targetInstallDir);
            }

            ZipFile.ExtractToDirectory(zipPath, targetInstallDir, overwriteFiles: true);
            return true;
        }
        catch
        {
            return false;
        }
    }
}
