using System;
using System.IO.Pipes;
using System.Security.Principal;
using System.Threading;
using System.Threading.Tasks;

namespace MockProvider;

public class PipeServer : IDisposable
{
    private readonly string _pipePrefix;
    private NamedPipeServerStream? _controlPipe;
    private NamedPipeServerStream? _audioPipe;

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

        _controlPipe = new NamedPipeServerStream(
            controlPipeName,
            PipeDirection.InOut,
            1,
            PipeTransmissionMode.Message,
            PipeOptions.Asynchronous);

        _audioPipe = new NamedPipeServerStream(
            audioPipeName,
            PipeDirection.Out,
            1,
            PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous);

        var controlTask = _controlPipe.WaitForConnectionAsync(cancellationToken);
        var audioTask = _audioPipe.WaitForConnectionAsync(cancellationToken);

        await Task.WhenAll(controlTask, audioTask);
    }

    public void Dispose()
    {
        _controlPipe?.Dispose();
        _audioPipe?.Dispose();
    }
}
