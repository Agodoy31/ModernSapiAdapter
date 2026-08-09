using System;
using System.IO;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace MockProvider;

class Program
{
    static async Task Main(string[] args)
    {
        string pipePrefix = "msa_mock_provider";
        if (args.Length > 0 && !args[0].StartsWith("--"))
        {
            pipePrefix = args[0];
        }
        for (int i = 0; i < args.Length; i++)
        {
            if (args[i] == "--pipe-name" && i + 1 < args.Length)
            {
                pipePrefix = args[i + 1];
            }
        }

        Console.WriteLine($"Starting MockProvider with pipe prefix: {pipePrefix}");

        using var pipeServer = new PipeServer(pipePrefix);
        var cts = new CancellationTokenSource();

        Console.CancelKeyPress += (s, e) =>
        {
            e.Cancel = true;
            cts.Cancel();
        };

        try
        {
            while (!cts.Token.IsCancellationRequested)
            {
                try
                {
                    await pipeServer.ListenAsync(cts.Token);
                    Console.WriteLine("Client connected.");

                    if (pipeServer.ControlPipe == null || pipeServer.AudioPipe == null)
                    {
                        Console.WriteLine("Error: Pipes are not initialized.");
                        break;
                    }

                    var synthesisEngine = new SynthesisEngine(pipeServer.ControlPipe, pipeServer.AudioPipe);
                    using var reader = new StreamReader(pipeServer.ControlPipe);
                    using var writer = new StreamWriter(pipeServer.ControlPipe) { AutoFlush = true };

                    CancellationTokenSource? speakCts = null;
                    Task? speakTask = null;

                    while (!cts.Token.IsCancellationRequested)
                    {
                        string? line = await reader.ReadLineAsync(cts.Token);
                        if (line == null) break;

                        try
                        {
                            using var doc = JsonDocument.Parse(line);
                            var root = doc.RootElement;
                            string command = root.GetProperty("command").GetString() ?? "";

                            if (command == "info")
                            {
                                var response = new
                                {
                                    audio_format = new
                                    {
                                        sample_rate = 24000,
                                        bits_per_sample = 16,
                                        channels = 1
                                    }
                                };
                                await writer.WriteLineAsync(JsonSerializer.Serialize(response));
                            }
                            else if (command == "voices")
                            {
                                var response = new
                                {
                                    voices = new[]
                                    {
                                        new { id = "mock_voice_1", name = "Mock Voice 1", language = "en-US", gender = "Female" },
                                        new { id = "mock_voice_2", name = "Mock Voice 2", language = "en-GB", gender = "Male" }
                                    }
                                };
                                await writer.WriteLineAsync(JsonSerializer.Serialize(response));
                            }
                            else if (command == "sapi_speak")
                            {
                                ulong speakId = root.GetProperty("speak_id").GetUInt64();
                                if (speakTask != null && !speakTask.IsCompleted)
                                {
                                    await writer.WriteLineAsync(JsonSerializer.Serialize(new
                                    {
                                        @event = "log",
                                        speak_id = speakId,
                                        severity = "error",
                                        message = "Another synthesis request is still active."
                                    }));
                                    continue;
                                }

                                speakCts = new CancellationTokenSource();
                                var fragments = root.GetProperty("fragments");
                                speakTask = synthesisEngine.HandleSpeakAsync(fragments.Clone(), speakId, speakCts.Token);
                            }
                            else if (command == "cancel")
                            {
                                speakCts?.Cancel();
                                if (speakTask != null)
                                {
                                    try { await speakTask; } catch { /* cancellation is acknowledged by the event. */ }
                                }
                            }
                            else
                            {
                                await writer.WriteLineAsync(JsonSerializer.Serialize(new { error = "Unknown command" }));
                            }
                        }
                        catch (Exception ex)
                        {
                            Console.WriteLine($"Error processing command: {ex.Message}");
                        }
                    }
                }
                catch (Exception ex)
                {
                    Console.WriteLine($"Connection loop error: {ex.Message}");
                    await Task.Delay(100);
                }
            }
        }
        catch (OperationCanceledException)
        {
            Console.WriteLine("Operation cancelled.");
        }
        catch (Exception ex)
        {
            Console.WriteLine($"Unhandled exception: {ex.Message}");
        }
    }
}
