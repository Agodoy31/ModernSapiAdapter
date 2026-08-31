using System;
using System.IO;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace MockProvider;

/// <summary>
/// Tagged enumeration for incoming provider commands over the control pipe.
/// </summary>
public enum ProviderCommand
{
    Info,
    Voices,
    Speak,
    Cancel,
    Unknown
}

class Program
{
    public static ProviderCommand ParseProviderCommand(string? command)
    {
        return command switch
        {
            "info" => ProviderCommand.Info,
            "voices" => ProviderCommand.Voices,
            "sapi_speak" => ProviderCommand.Speak,
            "cancel" => ProviderCommand.Cancel,
            _ => ProviderCommand.Unknown
        };
    }

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
        var lifetime = new ProviderLifetime(cts);

        Console.CancelKeyPress += (s, e) =>
        {
            e.Cancel = true;
            cts.Cancel();
        };

        lifetime.ArmIdleShutdown();

        try
        {
            while (!cts.Token.IsCancellationRequested)
            {
                PipeSession session = await pipeServer.AcceptSessionAsync(cts.Token);
                lifetime.SessionOpened();
                _ = HandleSessionAsync(session, cts.Token, lifetime.SessionClosed);
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

    private static async Task HandleSessionAsync(PipeSession session, CancellationToken cancellationToken, Action sessionClosed)
    {
        using var sessionOwner = session;
        using var reader = new StreamReader(session.ControlPipe);
        using var writer = new StreamWriter(session.ControlPipe) { AutoFlush = true };
        var synthesisEngine = new SynthesisEngine(session.ControlPipe, session.AudioPipe);
        var sessionContext = new SessionContext();

        try
        {
            await ReadSessionLoopAsync(reader, writer, synthesisEngine, sessionContext, cancellationToken);
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            Console.WriteLine($"Session error: {ex.Message}");
        }
        finally
        {
            sessionContext.SpeakCts?.Cancel();
            sessionClosed();
        }
    }

    private static async Task ReadSessionLoopAsync(
        StreamReader reader,
        StreamWriter writer,
        SynthesisEngine synthesisEngine,
        SessionContext sessionContext,
        CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            string? line = await reader.ReadLineAsync(cancellationToken);
            if (line == null)
            {
                break;
            }

            await ProcessSessionLineAsync(line, writer, synthesisEngine, sessionContext);
        }
    }

    private static async Task ProcessSessionLineAsync(
        string line,
        StreamWriter writer,
        SynthesisEngine synthesisEngine,
        SessionContext sessionContext)
    {
        try
        {
            using var doc = JsonDocument.Parse(line);
            var root = doc.RootElement;
            string command = root.GetProperty("command").GetString() ?? "";
            ProviderCommand providerCmd = ParseProviderCommand(command);

            switch (providerCmd)
            {
                case ProviderCommand.Info:
                {
                    await writer.WriteLineAsync(JsonSerializer.Serialize(new
                    {
                        response = "info",
                        audio_format = new { sample_rate = 24000, bits_per_sample = 16, channels = 1 }
                    }));
                    break;
                }

                case ProviderCommand.Voices:
                {
                    await writer.WriteLineAsync(JsonSerializer.Serialize(new
                    {
                        voices = new[]
                        {
                            new { id = "mock_voice_1", name = "Mock Voice 1", language = "en-US", gender = "Female" },
                            new { id = "mock_voice_2", name = "Mock Voice 2", language = "en-GB", gender = "Male" }
                        }
                    }));
                    break;
                }

                case ProviderCommand.Speak:
                {
                    await HandleSpeakCommandAsync(root, writer, synthesisEngine, sessionContext);
                    break;
                }

                case ProviderCommand.Cancel:
                {
                    await HandleCancelCommandAsync(sessionContext);
                    break;
                }

                case ProviderCommand.Unknown:
                default:
                {
                    await writer.WriteLineAsync(JsonSerializer.Serialize(new { error = "Unknown command" }));
                    break;
                }
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"Error processing command: {ex.Message}");
        }
    }

    private static async Task HandleSpeakCommandAsync(
        JsonElement root,
        StreamWriter writer,
        SynthesisEngine synthesisEngine,
        SessionContext sessionContext)
    {
        ulong speakId = root.GetProperty("speak_id").GetUInt64();
        if (sessionContext.SpeakTask != null && !sessionContext.SpeakTask.IsCompleted)
        {
            await writer.WriteLineAsync(JsonSerializer.Serialize(new
            {
                @event = "log",
                speak_id = speakId,
                severity = "error",
                message = "Another synthesis request is still active."
            }));
            return;
        }

        sessionContext.SpeakCts = new CancellationTokenSource();
        sessionContext.SpeakTask = synthesisEngine.HandleSpeakAsync(root.GetProperty("fragments").Clone(), speakId, sessionContext.SpeakCts.Token);
    }

    private static async Task HandleCancelCommandAsync(SessionContext sessionContext)
    {
        sessionContext.SpeakCts?.Cancel();
        if (sessionContext.SpeakTask == null)
        {
            return;
        }

        try
        {
            await sessionContext.SpeakTask;
        }
        catch
        {
        }
    }

    private sealed class SessionContext
    {
        public CancellationTokenSource? SpeakCts { get; set; }
        public Task? SpeakTask { get; set; }
    }

    private sealed class ProviderLifetime
    {
        private static readonly TimeSpan IdleTimeout = TimeSpan.FromSeconds(1);

        private readonly CancellationTokenSource _shutdown;
        private readonly object _sync = new();
        private CancellationTokenSource? _idleTimer;
        private int _activeSessions;

        public ProviderLifetime(CancellationTokenSource shutdown)
        {
            _shutdown = shutdown;
        }

        public void ArmIdleShutdown()
        {
            lock (_sync)
            {
                if (_activeSessions == 0)
                {
                    StartIdleTimerLocked();
                }
            }
        }

        public void SessionOpened()
        {
            lock (_sync)
            {
                _activeSessions++;
                _idleTimer?.Cancel();
                _idleTimer = null;
            }
        }

        public void SessionClosed()
        {
            lock (_sync)
            {
                _activeSessions--;
                if (_activeSessions == 0)
                {
                    StartIdleTimerLocked();
                }
            }
        }

        private void StartIdleTimerLocked()
        {
            _idleTimer?.Cancel();
            var timer = new CancellationTokenSource();
            _idleTimer = timer;
            _ = ShutdownWhenIdleAsync(timer);
        }

        private async Task ShutdownWhenIdleAsync(CancellationTokenSource timer)
        {
            try
            {
                await Task.Delay(IdleTimeout, timer.Token);
                TryTriggerShutdown(timer);
            }
            catch (OperationCanceledException)
            {
            }
            finally
            {
                timer.Dispose();
            }
        }

        private void TryTriggerShutdown(CancellationTokenSource timer)
        {
            lock (_sync)
            {
                if (ReferenceEquals(_idleTimer, timer) && _activeSessions == 0)
                {
                    _shutdown.Cancel();
                }
            }
        }
    }
}
