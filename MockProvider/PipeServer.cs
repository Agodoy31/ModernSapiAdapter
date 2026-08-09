using System;
using System.IO;
using System.IO.Pipes;
using System.Security.Principal;
using System.Threading;
using System.Threading.Tasks;

namespace MockProvider;

/// <summary>
/// Accepts independent control/audio pipe pairs for provider sessions.
/// </summary>
public sealed class PipeServer : IDisposable
{
    private readonly string _pipePrefix;

    public PipeServer(string pipePrefix)
    {
        _pipePrefix = pipePrefix;
    }

    /// <summary>
    /// Creates a fresh pair of server instances and waits until one client connects to both.
    /// </summary>
    public async Task<PipeSession> AcceptSessionAsync(CancellationToken cancellationToken = default)
    {
        string userSid = WindowsIdentity.GetCurrent().User?.Value ?? "DefaultUser";
        string baseName = $"{_pipePrefix}\\{userSid}";
        var controlPipe = new NamedPipeServerStream(
            $"{baseName}\\control",
            PipeDirection.InOut,
            NamedPipeServerStream.MaxAllowedServerInstances,
            PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous);
        var audioPipe = new NamedPipeServerStream(
            $"{baseName}\\audio",
            PipeDirection.Out,
            NamedPipeServerStream.MaxAllowedServerInstances,
            PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous);

        try
        {
            await Task.WhenAll(
                controlPipe.WaitForConnectionAsync(cancellationToken),
                audioPipe.WaitForConnectionAsync(cancellationToken));
            return new PipeSession(controlPipe, audioPipe);
        }
        catch
        {
            controlPipe.Dispose();
            audioPipe.Dispose();
            throw;
        }
    }

    public void Dispose()
    {
    }
}

/// <summary>
/// Owns the two streams belonging to a single provider client session.
/// </summary>
public sealed class PipeSession : IDisposable
{
    public PipeSession(NamedPipeServerStream controlPipe, NamedPipeServerStream audioPipe)
    {
        ControlPipe = controlPipe;
        AudioPipe = audioPipe;
    }

    public Stream ControlPipe { get; }
    public Stream AudioPipe { get; }

    public void Dispose()
    {
        ControlPipe.Dispose();
        AudioPipe.Dispose();
    }
}
