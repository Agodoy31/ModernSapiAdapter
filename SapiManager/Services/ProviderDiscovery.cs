using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Security.Principal;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;
using SapiManager.Models;

namespace SapiManager.Services;

public class ProviderDiscovery
{
    public async Task<ProviderProbeResult?> ProbeProviderAsync(string exePath)
    {
        if (string.IsNullOrWhiteSpace(exePath) || !File.Exists(exePath))
        {
            return null;
        }

        string dir = Path.GetDirectoryName(exePath)!;
        string manifestPath = Path.Combine(dir, "manifest.json");
        if (!File.Exists(manifestPath))
        {
            return null;
        }

        ProviderPackageManifest? manifest = null;
        try
        {
            string json = await File.ReadAllTextAsync(manifestPath);
            var jsonOptions = new JsonSerializerOptions { PropertyNameCaseInsensitive = true };
            manifest = JsonSerializer.Deserialize<ProviderPackageManifest>(json, jsonOptions);
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"Failed to parse provider manifest at '{manifestPath}': {ex.Message}");
            return null;
        }

        if (manifest == null || string.IsNullOrWhiteSpace(manifest.ProviderId))
        {
            return null;
        }

        string? userSid = WindowsIdentity.GetCurrent().User?.Value;
        if (string.IsNullOrEmpty(userSid))
        {
            return null;
        }

        string pipeName = $"{manifest.ProviderId}\\{userSid}\\control";

        Process? bgProcess = null;
        try
        {
            var startInfo = new ProcessStartInfo
            {
                FileName = exePath,
                UseShellExecute = false,
                CreateNoWindow = true
            };
            bgProcess = Process.Start(startInfo);
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"Failed to start process '{exePath}': {ex.Message}");
        }

        if (bgProcess == null || bgProcess.HasExited)
        {
            Debug.WriteLine($"Process '{exePath}' failed to start or exited immediately.");
            return null;
        }

        try
        {
            using var client = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
            await client.ConnectAsync(5000);

            var utf8NoBom = new UTF8Encoding(false);
            using var reader = new StreamReader(client, utf8NoBom, leaveOpen: true);
            using var writer = new StreamWriter(client, utf8NoBom, leaveOpen: true) { AutoFlush = true };

            // 1. Send info command
            var infoRequest = new InfoRequest();
            string infoJson = JsonSerializer.Serialize(infoRequest);
            await writer.WriteLineAsync(infoJson);

            string? infoResponseJson = await reader.ReadLineAsync();
            InfoResponse? infoResponse = null;
            if (!string.IsNullOrEmpty(infoResponseJson))
            {
                infoResponse = JsonSerializer.Deserialize<InfoResponse>(infoResponseJson);
            }

            // 2. Send voices command
            var voicesRequest = new VoicesRequest();
            string voicesJson = JsonSerializer.Serialize(voicesRequest);
            await writer.WriteLineAsync(voicesJson);

            string? voicesResponseJson = await reader.ReadLineAsync();
            VoicesResponse? voicesResponse = null;
            if (!string.IsNullOrEmpty(voicesResponseJson))
            {
                voicesResponse = JsonSerializer.Deserialize<VoicesResponse>(voicesResponseJson);
            }

            // 3. Send shutdown command
            try
            {
                var shutdownRequest = new ShutdownRequest();
                string shutdownJson = JsonSerializer.Serialize(shutdownRequest);
                await writer.WriteLineAsync(shutdownJson);
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"Failed to send shutdown command to provider '{exePath}': {ex.Message}");
            }

            return new ProviderProbeResult
            {
                ProviderId = manifest.ProviderId,
                ExePath = exePath,
                Info = infoResponse,
                Voices = voicesResponse?.Voices ?? new List<VoiceInfo>()
            };
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"Error probing provider '{exePath}': {ex.Message}");
            return null;
        }
        finally
        {
            if (bgProcess != null && !bgProcess.HasExited)
            {
                try { bgProcess.Kill(entireProcessTree: true); } catch (Exception ex) { Debug.WriteLine($"Error killing provider process '{exePath}': {ex.Message}"); }
            }
        }
    }
}
