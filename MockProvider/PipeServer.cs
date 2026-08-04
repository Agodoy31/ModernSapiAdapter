using System;
using System.IO.Pipes;
using System.Security.Principal;
using System.Threading;
using System.Threading.Tasks;

namespace MockProvider;

public class PipeServer : IDisposable
{
    private readonly string _pipePrefix;
    public NamedPipeServerStream? ControlPipe { get; private set; }
    public NamedPipeServerStream? AudioPipe { get; private set; }

    public PipeServer(string pipePrefix)
    {
        _pipePrefix = pipePrefix;
    }

    public async Task ListenAsync(CancellationToken cancellationToken = default)
    {
        string userSid = WindowsIdentity.GetCurrent().User?.Value ?? "DefaultUser";
        
        // Note: The NamedPipeServerStream constructor expects just the pipe name, not the \\.\pipe\ prefix.
        // If pipePrefix already contains it or the brief meant the full path was \\.\pipe\..., 
        // named pipes in .NET are created relative to \\.\pipe\
        string controlPipeName = $"{_pipePrefix}\\{userSid}\\control";
        string audioPipeName = $"{_pipePrefix}\\{userSid}\\audio";

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

    public void Dispose()
    {
        ControlPipe?.Dispose();
        AudioPipe?.Dispose();
    }
}
