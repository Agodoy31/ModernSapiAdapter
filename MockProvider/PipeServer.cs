using System;
using System.IO.Pipes;
using System.Security.Principal;
using System.Threading;
using System.Threading.Tasks;

namespace MockProvider;

/// <summary>
/// Manages asynchronous dual Named Pipe servers (Control and Audio) for the mock provider.
/// </summary>
public class PipeServer : IDisposable
{
    private readonly string _pipePrefix;

    /// <summary>
    /// Gets the Control Pipe server stream for bidirectional JSON messaging.
    /// </summary>
    public NamedPipeServerStream? ControlPipe { get; private set; }

    /// <summary>
    /// Gets the Audio Pipe server stream for outbound raw PCM streaming.
    /// </summary>
    public NamedPipeServerStream? AudioPipe { get; private set; }

    /// <summary>
    /// Initializes a new instance of the <see cref="PipeServer"/> class with the specified pipe prefix.
    /// </summary>
    /// <param name="pipePrefix">Base name prefix for named pipes.</param>
    public PipeServer(string pipePrefix)
    {
        _pipePrefix = pipePrefix;
    }

    /// <summary>
    /// Asynchronously listens for incoming client connections on both Control and Audio pipes.
    /// </summary>
    /// <param name="cancellationToken">Cancellation token to cancel connection waiting.</param>
    /// <returns>A task representing the asynchronous listen operation.</returns>
    public async Task ListenAsync(CancellationToken cancellationToken = default)
    {
        string userSid = WindowsIdentity.GetCurrent().User?.Value ?? "DefaultUser";
        
        string controlPipeName = $"{_pipePrefix}\\{userSid}\\control";
        string audioPipeName = $"{_pipePrefix}\\{userSid}\\audio";

        ControlPipe?.Dispose();
        AudioPipe?.Dispose();

        ControlPipe = new NamedPipeServerStream(
            controlPipeName,
            PipeDirection.InOut,
            1,
            PipeTransmissionMode.Message,
            PipeOptions.Asynchronous);

        AudioPipe = new NamedPipeServerStream(
            audioPipeName,
            PipeDirection.Out,
            1,
            PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous);

        var controlTask = ControlPipe.WaitForConnectionAsync(cancellationToken);
        var audioTask = AudioPipe.WaitForConnectionAsync(cancellationToken);

        await Task.WhenAll(controlTask, audioTask);
    }

    /// <summary>
    /// Disposes control and audio pipe streams.
    /// </summary>
    public void Dispose()
    {
        ControlPipe?.Dispose();
        AudioPipe?.Dispose();
    }
}
