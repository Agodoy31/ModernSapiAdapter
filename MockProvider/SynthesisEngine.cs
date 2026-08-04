using System;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace MockProvider;

public class SynthesisEngine
{
    private readonly Stream _controlPipe;
    private readonly Stream _audioPipe;

    public SynthesisEngine(Stream controlPipe, Stream audioPipe)
    {
        _controlPipe = controlPipe;
        _audioPipe = audioPipe;
    }

    public byte[] GenerateSineWave(int durationMs, double frequency = 440.0)
    {
        int sampleRate = 24000;
        int totalSamples = (int)((durationMs / 1000.0) * sampleRate);
        byte[] pcmData = new byte[totalSamples * 2]; // 16-bit Mono

        for (int i = 0; i < totalSamples; i++)
        {
            double time = i / (double)sampleRate;
            short sample = (short)(Math.Sin(2 * Math.PI * frequency * time) * short.MaxValue);
            byte[] sampleBytes = BitConverter.GetBytes(sample);
            pcmData[i * 2] = sampleBytes[0];
            pcmData[i * 2 + 1] = sampleBytes[1];
        }

        return pcmData;
    }

    public async Task HandleSpeakAsync(JsonElement fragments, CancellationToken cancellationToken)
    {
        try
        {
            foreach (var fragment in fragments.EnumerateArray())
            {
                cancellationToken.ThrowIfCancellationRequested();

                string type = fragment.GetProperty("type").GetString() ?? "";
                if (type == "text")
                {
                    string text = fragment.GetProperty("text").GetString() ?? "";
                    string[] words = text.Split(new[] { ' ', '\t', '\n', '\r' }, StringSplitOptions.RemoveEmptyEntries);
                    foreach (var word in words)
                    {
                        cancellationToken.ThrowIfCancellationRequested();

                        var wordBoundary = new { @event = "word_boundary", text = word };
                        await WriteControlEventAsync(wordBoundary, cancellationToken);

                        byte[] audioData = GenerateSineWave(200);
                        await _audioPipe.WriteAsync(audioData, cancellationToken);
                    }
                }
                else if (type == "silence")
                {
                    int durationMs = fragment.GetProperty("duration_ms").GetInt32();
                    byte[] audioData = new byte[(int)((durationMs / 1000.0) * 24000) * 2];
                    await _audioPipe.WriteAsync(audioData, cancellationToken);
                }
                else if (type == "bookmark")
                {
                    string name = fragment.GetProperty("name").GetString() ?? "";
                    var bookmarkEvent = new { @event = "bookmark_reached", name = name };
                    await WriteControlEventAsync(bookmarkEvent, cancellationToken);
                }
            }

            var completedEvent = new { @event = "completed" };
            await WriteControlEventAsync(completedEvent, cancellationToken);
        }
        catch (OperationCanceledException)
        {
            var cancelledEvent = new { @event = "cancelled" };
            await WriteControlEventAsync(cancelledEvent, CancellationToken.None);
        }
        catch (Exception ex)
        {
            var errorEvent = new { @event = "error", message = ex.Message };
            await WriteControlEventAsync(errorEvent, CancellationToken.None);
        }
    }

    private async Task WriteControlEventAsync(object evt, CancellationToken cancellationToken)
    {
        string json = JsonSerializer.Serialize(evt);
        byte[] bytes = Encoding.UTF8.GetBytes(json + "\n");
        await _controlPipe.WriteAsync(bytes, cancellationToken);
        await _controlPipe.FlushAsync(cancellationToken);
    }
}
