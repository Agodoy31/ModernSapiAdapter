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
            manifest = JsonSerializer.Deserialize(json, SapiJsonContext.Default.ProviderPackageManifest);
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

            // 1. Send info command
            var infoRequest = new InfoRequest();
            byte[] infoBytes = JsonSerializer.SerializeToUtf8Bytes(infoRequest, SapiJsonContext.Default.InfoRequest);
            await client.WriteAsync(infoBytes);
            client.WriteByte((byte)'\n');
            await client.FlushAsync();

            string? infoResponseJson = await ReadLineUtf8Async(client);
            InfoResponse? infoResponse = null;
            if (!string.IsNullOrEmpty(infoResponseJson))
            {
                infoResponse = JsonSerializer.Deserialize(infoResponseJson, SapiJsonContext.Default.InfoResponse);
            }

            // 2. Send voices command
            var voicesRequest = new VoicesRequest();
            byte[] voicesBytes = JsonSerializer.SerializeToUtf8Bytes(voicesRequest, SapiJsonContext.Default.VoicesRequest);
            await client.WriteAsync(voicesBytes);
            client.WriteByte((byte)'\n');
            await client.FlushAsync();

            string? voicesResponseJson = await ReadLineUtf8Async(client);
            VoicesResponse? voicesResponse = null;
            if (!string.IsNullOrEmpty(voicesResponseJson))
            {
                voicesResponse = JsonSerializer.Deserialize(voicesResponseJson, SapiJsonContext.Default.VoicesResponse);
            }

            // 3. Send shutdown command
            try
            {
                var shutdownRequest = new ShutdownRequest();
                byte[] shutdownBytes = JsonSerializer.SerializeToUtf8Bytes(shutdownRequest, SapiJsonContext.Default.ShutdownRequest);
                await client.WriteAsync(shutdownBytes);
                client.WriteByte((byte)'\n');
                await client.FlushAsync();
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

    private static async Task<string?> ReadLineUtf8Async(NamedPipeClientStream stream)
    {
        using var ms = new MemoryStream();
        byte[] buffer = System.Buffers.ArrayPool<byte>.Shared.Rent(65536);
        try
        {
            while (true)
            {
                int bytesRead = await stream.ReadAsync(buffer, 0, buffer.Length);
                if (bytesRead <= 0) break;

                int newlineIndex = Array.IndexOf(buffer, (byte)'\n', 0, bytesRead);
                if (newlineIndex >= 0)
                {
                    int lengthToWrite = newlineIndex;
                    if (lengthToWrite > 0 && buffer[lengthToWrite - 1] == (byte)'\r')
                    {
                        lengthToWrite--;
                    }
                    if (lengthToWrite > 0)
                    {
                        ms.Write(buffer, 0, lengthToWrite);
                    }
                    break;
                }
                else
                {
                    ms.Write(buffer, 0, bytesRead);
                }
            }

            return ms.Length > 0 ? Encoding.UTF8.GetString(ms.ToArray()) : null;
        }
        finally
        {
            System.Buffers.ArrayPool<byte>.Shared.Return(buffer);
        }
    }
}
