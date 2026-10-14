using System;
using System.IO;
using System.Text;
using System.Collections.Generic;
namespace Vinyl2000.App;

// Advisory inspection only. Never changes the file or excludes a source.
internal static class ForensicCheck
{
    internal static string Inspect(string path)
    {
        var report=new StringBuilder();report.AppendLine(Path.GetFileName(path));
        using var fs=File.OpenRead(path);using var br=new BinaryReader(fs,Encoding.ASCII);
        if(fs.Length<12 || Encoding.ASCII.GetString(br.ReadBytes(4))!="RIFF")
            return report+"Not a RIFF WAV: original-file inspection unavailable.\n";
        br.ReadUInt32();if(Encoding.ASCII.GetString(br.ReadBytes(4))!="WAVE")return report+"Not WAVE.\n";
        ushort format=0,channels=0,bits=0,align=0;uint rate=0;long dataStart=0,dataSize=0;
        bool metadataHit=false;var chunks=new List<string>();
        while(fs.Position+8<=fs.Length)
        {
            string id=Encoding.ASCII.GetString(br.ReadBytes(4));uint size=br.ReadUInt32();long start=fs.Position;
            if(start+size>fs.Length)throw new InvalidDataException("Truncated WAV chunk");
            if(id=="fmt " && size>=16){format=br.ReadUInt16();channels=br.ReadUInt16();rate=br.ReadUInt32();br.ReadUInt32();align=br.ReadUInt16();bits=br.ReadUInt16();}
            else if(id=="data"){dataStart=start;dataSize=size;}
            else if(id=="bext" || id=="iXML" || id=="LIST" || id=="axml")
            {
                chunks.Add(id);string text=Encoding.UTF8.GetString(br.ReadBytes((int)Math.Min(size,1048576u)));
                foreach(string word in new[]{"iZotope","declick","de-click","de click","interpolat","spectral repair"})
                    if(text.IndexOf(word,StringComparison.OrdinalIgnoreCase)>=0){metadataHit=true;report.AppendLine("Metadata keyword: "+word+" ("+id+"). Software names alone do not establish processing.");}
            }
            fs.Position=Math.Min(fs.Length,start+size+(size&1));
        }
        report.AppendLine("Metadata chunks: "+(chunks.Count==0?"none":string.Join(", ",chunks)));
        if(!metadataHit)report.AppendLine("No processing keywords found in inspected metadata (max 1 MiB per chunk).");
        if(format!=1 || channels<1 || channels>2 || rate==0 || (bits!=16 && bits!=24 && bits!=32) || align!=channels*(bits/8) || dataStart==0)
            return report+"Signal test unavailable for this WAV encoding. No conclusion about processing.\n";
        long frames=dataSize/align;int count=(int)Math.Min(frames,Math.Min(rate/2,96000u));
        if(count<16)return report+"Too short for signal inspection.\n";
        int windows=frames<=count?1:12,hits=0;var examples=new List<string>();
        double ReadSample(){if(bits==16)return br.ReadInt16();if(bits==32)return br.ReadInt32();int n=br.ReadByte()|(br.ReadByte()<<8)|(br.ReadByte()<<16);return (n&0x800000)!=0?n-0x1000000:n;}
        double scale=Math.Pow(2,bits-1);
        for(int w=0;w<windows;++w)
        {
            long first=windows==1?0:(frames-count)*w/(windows-1);fs.Position=dataStart+first*align;
            var a=new double[channels,count];for(int i=0;i<count;++i)for(int ch=0;ch<channels;++ch)a[ch,i]=ReadSample();
            for(int ch=0;ch<channels;++ch){int run=0;
                for(int i=2;i<count;++i){double d=a[ch,i]-a[ch,i-1],prev=a[ch,i-1]-a[ch,i-2];
                    bool linear=Math.Abs(d-prev)<=2 && Math.Abs(d)>=Math.Max(4,scale*0.00001);
                    run=linear?run+1:0;
                    if(run==12){hits++;if(examples.Count<8)examples.Add($"{(first+i-12)/(double)rate:0.000}s ch {ch+1}");}
                }
            }
        }
        report.AppendLine($"Sampled {windows} windows of {count/(double)rate:0.00}s across file. Near-linear non-flat runs: {hits}.");
        if(hits>0)report.AppendLine("Review candidates: "+string.Join(", ",examples)+". Natural signals, resampling and quantization can also produce these patterns.");
        report.AppendLine("Conclusion: "+(hits>0||metadataHit?"indications to review; de-clicking NOT proven.":"no indication in these tests; raw/unprocessed status NOT proven."));
        report.AppendLine("LPC, ultrasonic-hole and phase/group-delay tests are not implemented in this beta.");
        return report.ToString();
    }
}
