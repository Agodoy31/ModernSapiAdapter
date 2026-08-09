using System;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace MockProvider;

/// <summary>
/// Simulates speech synthesis, PCM sine wave audio generation, and JSON event emission.
/// </summary>
public class SynthesisEngine
{
    private readonly Stream _controlPipe;
    private readonly Stream _audioPipe;

    /// <summary>
    /// Initializes a new instance of the <see cref="SynthesisEngine"/> class.
    /// </summary>
    /// <param name="controlPipe">Control Pipe stream for JSON event messaging.</param>
    /// <param name="audioPipe">Audio Pipe stream for raw PCM output.</param>
    public SynthesisEngine(Stream controlPipe, Stream audioPipe)
    {
        _controlPipe = controlPipe;
        _audioPipe = audioPipe;
    }

    /// <summary>
    /// Generates raw 24kHz 16-bit Mono PCM sine wave audio byte buffers.
    /// </summary>
    /// <param name="durationMs">Audio duration in milliseconds.</param>
    /// <param name="frequency">Sine wave frequency in Hertz (default 440 Hz).</param>
    /// <returns>Byte array containing raw PCM sample data.</returns>
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

    /// <summary>
    /// Processes a list of speech fragments, writing audio to Audio Pipe and events to Control Pipe.
    /// </summary>
    /// <param name="fragments">JSON array of speech text or tuning fragments.</param>
    /// <param name="speakId">Identifier that associates each emitted event with its request.</param>
    /// <param name="cancellationToken">Cancellation token for speech cancellation.</param>
    /// <returns>A task representing the asynchronous speak execution.</returns>
    public async Task HandleSpeakAsync(JsonElement fragments, ulong speakId, CancellationToken cancellationToken)
    {
        long audioBytesWritten = 0;
        try
        {
            int totalAudioMs = 0;
            long totalAudioBytes = 0;
            var audioBuffers = new List<byte[]>();
            bool firstAudioBuffer = true;
            foreach (var fragment in fragments.EnumerateArray())
            {
                cancellationToken.ThrowIfCancellationRequested();

                if (fragment.TryGetProperty("text", out var textProp))
                {
                    string text = textProp.GetString() ?? "";
                    string[] words = text.Split(new[] { ' ', '\t', '\n', '\r' }, StringSplitOptions.RemoveEmptyEntries);
                    uint charOffset = fragment.TryGetProperty("source_offset", out var sourceOffsetProp)
                        ? sourceOffsetProp.GetUInt32()
                        : 0;
                    foreach (var word in words)
                    {
                        cancellationToken.ThrowIfCancellationRequested();

                        var wordBoundary = new
                        {
                            @event = "word_boundary",
                            speak_id = speakId,
                            text_offset = charOffset,
                            text_length = word.Length,
                            audio_offset_ms = totalAudioMs
                        };
                        await WriteControlEventAsync(wordBoundary, cancellationToken);

                        await Task.Delay(50, cancellationToken);

                        byte[] audioData = GenerateSineWave(200);
                        if (firstAudioBuffer)
                        {
                            await _audioPipe.WriteAsync(audioData, cancellationToken);
                            await _audioPipe.FlushAsync(cancellationToken);
                            audioBytesWritten += audioData.Length;
                            firstAudioBuffer = false;
                        }
                        else
                        {
                            audioBuffers.Add(audioData);
                        }
                        totalAudioBytes += audioData.Length;

                        totalAudioMs += 200;
                        charOffset += (uint)(word.Length + 1);
                    }
                }
                else if (fragment.TryGetProperty("bookmark", out var bookmarkProp))
                {
                    var bookmarkEvent = new
                    {
                        @event = "bookmark_reached",
                        speak_id = speakId,
                        bookmark_name = bookmarkProp.GetString() ?? "",
                        audio_offset_ms = totalAudioMs
                    };
                    await WriteControlEventAsync(bookmarkEvent, cancellationToken);
                }
            }

            var completionEvent = new
            {
                @event = "synthesis_complete",
                speak_id = speakId,
                total_audio_bytes = totalAudioBytes
            };
            await WriteControlEventAsync(completionEvent, cancellationToken);

            foreach (byte[] audioData in audioBuffers)
            {
                await _audioPipe.WriteAsync(audioData, cancellationToken);
                await _audioPipe.FlushAsync(cancellationToken);
                audioBytesWritten += audioData.Length;
            }
        }
        catch (OperationCanceledException)
        {
            // Providers can have event callbacks already queued when cancellation arrives.
            // The CoreEngine regression test verifies these cannot reach SAPI after abort.
            var lateSentenceBoundary = new
            {
                @event = "sentence_boundary",
                speak_id = speakId,
                text_offset = 0,
                text_length = 1,
                audio_offset_ms = 0
            };
            await WriteControlEventAsync(lateSentenceBoundary, CancellationToken.None);

            var cancelledEvent = new
            {
                @event = "synthesis_cancelled",
                speak_id = speakId,
                audio_bytes_written = audioBytesWritten
            };
            await WriteControlEventAsync(cancelledEvent, CancellationToken.None);
        }
        catch (Exception ex)
        {
            var logEvent = new { @event = "log", speak_id = speakId, severity = "error", message = ex.Message };
            await WriteControlEventAsync(logEvent, CancellationToken.None);
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
