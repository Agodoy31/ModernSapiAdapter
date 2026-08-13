using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text.Json;
using SapiManager.Models;

namespace SapiManager.Services;

public class ProviderInstaller
{
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

            var manifestEntry = archive.Entries.FirstOrDefault(e =>
                e.Name.Equals("manifest.json", StringComparison.OrdinalIgnoreCase));

            if (manifestEntry != null)
            {
                using var stream = manifestEntry.Open();
                manifest = JsonSerializer.Deserialize(stream, SapiJsonContext.Default.ProviderPackageManifest);
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
                ErrorMessage = "Invalid package: Missing mandatory manifest.json file."
            };
        }

        string targetInstallDir = Path.Combine(baseProvidersDir, manifest.ProviderId);
        string? existingVersion = null;

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

    public bool TerminateProcess(int processId)
    {
        if (processId <= 0) return false;

        try
        {
            using var proc = Process.GetProcessById(processId);
            if (proc.HasExited) return true;

            proc.Kill(entireProcessTree: true);
            proc.WaitForExit(3000);
            return proc.HasExited;
        }
        catch (ArgumentException)
        {
            return true;
        }
        catch (Exception)
        {
            return false;
        }
    }

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

            using var archive = ZipFile.OpenRead(zipPath);

            var validEntries = archive.Entries.Where(e => !string.IsNullOrEmpty(e.Name)).ToList();
            string? singleRootPrefix = null;

            if (validEntries.Count > 0)
            {
                string firstFullName = validEntries[0].FullName.Replace('\\', '/');
                int slashIdx = firstFullName.IndexOf('/');
                if (slashIdx > 0)
                {
                    string candidatePrefix = firstFullName.Substring(0, slashIdx + 1);
                    if (archive.Entries.All(e => e.FullName.Replace('\\', '/').StartsWith(candidatePrefix, StringComparison.OrdinalIgnoreCase)))
                    {
                        singleRootPrefix = candidatePrefix;
                    }
                }
            }

            foreach (var entry in archive.Entries)
            {
                if (string.IsNullOrEmpty(entry.Name)) continue;

                string relativePath = entry.FullName.Replace('\\', '/');
                if (singleRootPrefix != null && relativePath.StartsWith(singleRootPrefix, StringComparison.OrdinalIgnoreCase))
                {
                    relativePath = relativePath.Substring(singleRootPrefix.Length);
                }

                if (string.IsNullOrWhiteSpace(relativePath)) continue;

                string destPath = Path.Combine(targetInstallDir, relativePath.Replace('/', Path.DirectorySeparatorChar));
                string? destDir = Path.GetDirectoryName(destPath);
                if (destDir != null && !Directory.Exists(destDir))
                {
                    Directory.CreateDirectory(destDir);
                }

                entry.ExtractToFile(destPath, overwrite: true);
            }

            return true;
        }
        catch
        {
            return false;
        }
    }
}
