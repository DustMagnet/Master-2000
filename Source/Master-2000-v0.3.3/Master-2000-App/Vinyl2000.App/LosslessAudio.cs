using NAudio.Wave;

namespace Vinyl2000.App;

internal static class LosslessAudio
{
    internal static readonly string[] Extensions =
    [
        ".wav", ".wave", ".flac", ".aif", ".aiff", ".aifc", ".m4a", ".alac", ".wma"
    ];

    internal static bool IsSupportedPath(string path) =>
        Extensions.Contains(System.IO.Path.GetExtension(path),StringComparer.OrdinalIgnoreCase);

    internal static async Task<(string EnginePath,WavFormat Format,string Description)> PrepareAsync(
        string sourcePath,string stableHash)
    {
        // Native PCM WAV remains byte-for-byte untouched.
        var direct=WavInfo.Read(sourcePath);
        if(direct is not null)
            return (sourcePath,direct,WavInfo.Describe(sourcePath));

        if(!IsSupportedPath(sourcePath))
            throw new InvalidDataException("Unsupported audio format: "+System.IO.Path.GetExtension(sourcePath));

        string cacheDir=System.IO.Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "Master-2000","LosslessCache");
        Directory.CreateDirectory(cacheDir);
        string pcmPath=System.IO.Path.Combine(cacheDir,stableHash+".pcm24.wav");

        await Task.Run(() => DecodeToPcm24(sourcePath,pcmPath));
        var format=WavInfo.Read(pcmPath) ??
            throw new InvalidDataException("The lossless decoder did not produce a valid PCM WAV.");
        string codec=System.IO.Path.GetExtension(sourcePath).TrimStart('.').ToUpperInvariant();
        string duration=format.Seconds>0 ? TimeSpan.FromSeconds(format.Seconds).ToString(@"m\:ss\.fff") : "";
        string description=$"{format.SampleRate/1000.0:0.###} kHz • {format.Bits}-bit • {format.ChannelText}"+
            (duration.Length>0?$" • {duration}":"")+$" • imported from {codec}";
        return (pcmPath,format,description);
    }

    private static void DecodeToPcm24(string input,string output)
    {
        string ext=System.IO.Path.GetExtension(input).ToLowerInvariant();
        using WaveStream reader=ext is ".aif" or ".aiff" or ".aifc"
            ? new AiffFileReader(input)
            : new MediaFoundationReader(input,new MediaFoundationReader.MediaFoundationReaderSettings
            {
                RequestFloatOutput=true
            });
        if(reader.WaveFormat.Channels is <1 or >2)
            throw new InvalidDataException("Only mono and stereo lossless audio is supported.");

        ISampleProvider samples=reader.ToSampleProvider();
        var outputFormat=new WaveFormat(reader.WaveFormat.SampleRate,24,reader.WaveFormat.Channels);
        using var writer=new WaveFileWriter(output,outputFormat);
        float[] floats=new float[16384*reader.WaveFormat.Channels];
        byte[] pcm=new byte[floats.Length*3];
        int read;
        while((read=samples.Read(floats,0,floats.Length))>0)
        {
            for(int i=0;i<read;++i)
            {
                double clamped=Math.Clamp((double)floats[i],-1.0,0.9999998807907104);
                int value=(int)Math.Round(clamped*8388608.0);
                int o=i*3;
                pcm[o]=(byte)value;
                pcm[o+1]=(byte)(value>>8);
                pcm[o+2]=(byte)(value>>16);
            }
            writer.Write(pcm,0,read*3);
        }
    }
}
