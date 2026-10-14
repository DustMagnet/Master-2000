using System.Text;
namespace Vinyl2000.App;

public sealed record WavFormat(uint SampleRate, ushort Bits, ushort Channels, double Seconds)
{
    public string ChannelText => Channels == 1 ? "mono" : Channels == 2 ? "stereo" : $"{Channels} ch";
    public string Short => $"{SampleRate/1000.0:0.###} kHz / {Bits}-bit / {ChannelText}";
}

internal static class WavInfo
{
    internal static WavFormat? Read(string path)
    {
        try
        {
            using var fs = File.OpenRead(path);
            using var br = new BinaryReader(fs, Encoding.ASCII, false);
            if (new string(br.ReadChars(4)) != "RIFF") return null;
            br.ReadUInt32();
            if (new string(br.ReadChars(4)) != "WAVE") return null;
            ushort channels=0,bits=0; uint rate=0; long dataBytes=0; ushort format=0;
            while (fs.Position + 8 <= fs.Length)
            {
                string id = new string(br.ReadChars(4)); uint size = br.ReadUInt32();
                long next = fs.Position + size + (size & 1);
                if (id == "fmt " && size >= 16)
                {
                    format=br.ReadUInt16(); channels=br.ReadUInt16(); rate=br.ReadUInt32(); br.ReadUInt32(); br.ReadUInt16(); bits=br.ReadUInt16();
                }
                else if (id == "data") dataBytes=size;
                fs.Position=Math.Min(next, fs.Length);
            }
            if(format != 1 || rate==0 || channels==0 || bits==0) return null;
            double sec=dataBytes/(rate*channels*(bits/8.0));
            return new WavFormat(rate,bits,channels,sec);
        }
        catch { return null; }
    }

    internal static string Describe(string path)
    {
        var f=Read(path);
        if(f is null) return "Unsupported or invalid WAV";
        string dur=f.Seconds>0?TimeSpan.FromSeconds(f.Seconds).ToString(@"m\:ss\.fff"):"";
        return $"{f.SampleRate/1000.0:0.###} kHz • {f.Bits}-bit • {f.ChannelText}" + (dur.Length>0?$" • {dur}":"");
    }
}
