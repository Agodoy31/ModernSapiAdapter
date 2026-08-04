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

namespace SapiManager.Services
{
    public class ProviderProber
    {
        public async Task<string?> GetProviderIdAsync(string exePath)
        {
            if (string.IsNullOrWhiteSpace(exePath) || !File.Exists(exePath))
            {
                return null;
            }

            try
            {
                var startInfo = new ProcessStartInfo
                {
                    FileName = exePath,
                    Arguments = "/pipe",
                    UseShellExecute = false,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    CreateNoWindow = true
                };

                using var process = new Process { StartInfo = startInfo };
                process.Start();

                string? line = await process.StandardOutput.ReadLineAsync();
                return line?.Trim();
            }
            catch
            {
                return null;
            }
        }

        public async Task<ProviderProbeResult?> ProbeProviderAsync(string exePath)
        {
            string? providerId = await GetProviderIdAsync(exePath);
            if (string.IsNullOrWhiteSpace(providerId))
            {
                return null;
            }

            string? userSid = WindowsIdentity.GetCurrent().User?.Value;
            if (string.IsNullOrEmpty(userSid))
            {
                return null;
            }

            string pipeName = $"{providerId}\\{userSid}\\control";

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
                catch
                {
                    // Shutdown command sent
                }

                return new ProviderProbeResult
                {
                    ProviderId = providerId,
                    ExePath = exePath,
                    Info = infoResponse,
                    Voices = voicesResponse?.Voices ?? new List<VoiceInfo>()
                };
            }
            catch
            {
                return null;
            }
        }
    }
}
