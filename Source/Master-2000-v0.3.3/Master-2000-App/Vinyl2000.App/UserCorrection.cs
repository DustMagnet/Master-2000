using System.Globalization;
using System.Text;

namespace Vinyl2000.App;

internal static class UserCorrection
{
    private sealed record PcmLayout(long DataOffset,long DataBytes,uint SampleRate,
        ushort Channels,ushort Bits,ushort BlockAlign,ushort Format);

    internal static string MasterOnlyPath(string resultPath)
    {
        string directory=Path.GetDirectoryName(resultPath) ?? "";
        string stem=Path.GetFileNameWithoutExtension(resultPath);
        return Path.Combine(directory,stem+".master-only.wav");
    }

    private static PcmLayout ReadLayout(Stream stream)
    {
        stream.Position=0;
        using var reader=new BinaryReader(stream,Encoding.ASCII,true);
        if(new string(reader.ReadChars(4))!="RIFF")
            throw new InvalidDataException("The correction source is not a RIFF WAV file.");
        _=reader.ReadUInt32();
        if(new string(reader.ReadChars(4))!="WAVE")
            throw new InvalidDataException("The correction source is not a WAVE file.");
        ushort format=0,channels=0,bits=0,blockAlign=0;
        uint sampleRate=0;
        long dataOffset=-1,dataBytes=0;
        while(stream.Position+8<=stream.Length)
        {
            string id=new string(reader.ReadChars(4));
            uint size=reader.ReadUInt32();
            long payload=stream.Position;
            long next=Math.Min(stream.Length,payload+size+(size&1));
            if(id=="fmt " && size>=16)
            {
                format=reader.ReadUInt16();
                channels=reader.ReadUInt16();
                sampleRate=reader.ReadUInt32();
                _=reader.ReadUInt32();
                blockAlign=reader.ReadUInt16();
                bits=reader.ReadUInt16();
            }
            else if(id=="data")
            {
                dataOffset=payload;
                dataBytes=Math.Min((long)size,stream.Length-payload);
            }
            stream.Position=next;
        }
        if(format!=1 || channels==0 || sampleRate==0 || bits is not (8 or 16 or 24 or 32) ||
           blockAlign==0 || dataOffset<0 || dataBytes<=0)
            throw new InvalidDataException("Only ordinary 8/16/24/32-bit PCM WAV files can be corrected.");
        return new PcmLayout(dataOffset,dataBytes,sampleRate,channels,bits,blockAlign,format);
    }

    // Copy the WAV verbatim, then change only the explicitly approved prefix.
    internal static void SaveStartBlank(string source,string destination,double fullLevelSeconds)
    {
        if(string.Equals(Path.GetFullPath(source),Path.GetFullPath(destination),StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Choose a new file; the engine result is protected.");
        using(var input=File.OpenRead(source))
        {
            var layout=ReadLayout(input);
            if(!double.IsFinite(fullLevelSeconds) || fullLevelSeconds<0.020 ||
               fullLevelSeconds>Math.Min(10.0,layout.DataBytes/(double)layout.BlockAlign/layout.SampleRate))
                throw new InvalidDataException("Choose a full-level boundary between 0.020 s and 10 s, inside the result.");
        }
        File.Copy(source,destination,true);
        using(var output=File.Open(destination,FileMode.Open,FileAccess.ReadWrite,FileShare.None))
        {
            var layout=ReadLayout(output);
            int count=checked((int)Math.Round(fullLevelSeconds*layout.SampleRate));
            int fade=Math.Max(1,(int)Math.Round(0.020*layout.SampleRate));
            int zero=Math.Max(0,count-fade),bytesPerSample=layout.Bits/8;
            byte[] bytes=new byte[checked(count*layout.BlockAlign)];
            output.Position=layout.DataOffset;output.ReadExactly(bytes);
            for(int frame=0;frame<count;++frame)
            {
                double u=frame<zero?0:(frame-zero)/(double)fade;
                double gain=u*u*u*(10+u*(-15+6*u));
                for(int channel=0;channel<layout.Channels;++channel)
                {
                    int offset=frame*layout.BlockAlign+channel*bytesPerSample;
                    long sample=0;
                    for(int b=0;b<bytesPerSample;++b)sample|=(long)bytes[offset+b]<<(8*b);
                    if(layout.Bits==8)sample-=128;
                    else if((sample & (1L<<(layout.Bits-1)))!=0)sample-=1L<<layout.Bits;
                    long value=(long)Math.Round(sample*gain);
                    if(layout.Bits==8)value+=128;
                    for(int b=0;b<bytesPerSample;++b)bytes[offset+b]=(byte)((value>>(8*b))&255);
                }
            }
            output.Position=layout.DataOffset;output.Write(bytes);
        }
        File.WriteAllText(Path.ChangeExtension(destination,".start-blank.txt"),
            "User-approved start blanking\nSource: "+Path.GetFileName(source)+
            "\nFull level from (seconds): "+fullLevelSeconds.ToString("R",CultureInfo.InvariantCulture)+
            "\nFade: 0.020 seconds. Later PCM bytes unchanged.\n",Encoding.UTF8);
    }

    private static List<(long Start,long End)> MergeRanges(
        IEnumerable<DecisionMapData.RepairInterval> repairs,
        PcmLayout layout,double speedFactor)
    {
        double factor=Math.Max(0.000001,speedFactor);
        long frameCount=layout.DataBytes/layout.BlockAlign;
        var ranges=repairs.Select(repair=>(
                Start:Math.Clamp((long)Math.Floor(repair.Start*layout.SampleRate/factor),0,frameCount),
                End:Math.Clamp((long)Math.Ceiling(repair.End*layout.SampleRate/factor),0,frameCount)))
            .Where(range=>range.End>range.Start)
            .OrderBy(range=>range.Start)
            .ToList();
        var merged=new List<(long Start,long End)>();
        foreach(var range in ranges)
        {
            if(merged.Count==0 || range.Start>merged[^1].End)
                merged.Add(range);
            else
                merged[^1]=(merged[^1].Start,Math.Max(merged[^1].End,range.End));
        }
        return merged;
    }

    internal static long Save(string resultPath,string masterOnlyPath,string destination,
        IEnumerable<DecisionMapData.RepairInterval> repairs,double speedFactor)
    {
        string resultFull=Path.GetFullPath(resultPath);
        string masterFull=Path.GetFullPath(masterOnlyPath);
        string destinationFull=Path.GetFullPath(destination);
        if(string.Equals(destinationFull,resultFull,StringComparison.OrdinalIgnoreCase) ||
           string.Equals(destinationFull,masterFull,StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Choose a new filename; the original result and Master comparison are protected.");

        Directory.CreateDirectory(Path.GetDirectoryName(destinationFull)!);
        File.Copy(resultFull,destinationFull,true);
        try
        {
            using var master=File.Open(masterFull,FileMode.Open,FileAccess.Read,FileShare.Read);
            using var corrected=File.Open(destinationFull,FileMode.Open,FileAccess.ReadWrite,FileShare.None);
            PcmLayout masterLayout=ReadLayout(master);
            PcmLayout correctedLayout=ReadLayout(corrected);
            if(masterLayout.SampleRate!=correctedLayout.SampleRate ||
               masterLayout.Channels!=correctedLayout.Channels ||
               masterLayout.Bits!=correctedLayout.Bits ||
               masterLayout.BlockAlign!=correctedLayout.BlockAlign ||
               masterLayout.DataBytes!=correctedLayout.DataBytes)
                throw new InvalidDataException("Result and Master-only WAV formats do not match.");

            var selected=repairs.ToList();
            var ranges=MergeRanges(selected,correctedLayout,speedFactor);
            byte[] buffer=new byte[1024*1024];
            long restoredFrames=0;
            foreach(var range in ranges)
            {
                long bytes=(range.End-range.Start)*correctedLayout.BlockAlign;
                master.Position=masterLayout.DataOffset+range.Start*masterLayout.BlockAlign;
                corrected.Position=correctedLayout.DataOffset+range.Start*correctedLayout.BlockAlign;
                while(bytes>0)
                {
                    int wanted=(int)Math.Min(buffer.Length,bytes);
                    int read=master.Read(buffer,0,wanted);
                    if(read<=0) throw new EndOfStreamException("Master-only WAV ended inside a correction interval.");
                    corrected.Write(buffer,0,read);
                    bytes-=read;
                }
                restoredFrames+=range.End-range.Start;
            }
            corrected.Flush(true);

            string logPath=Path.Combine(Path.GetDirectoryName(destinationFull)!,
                Path.GetFileNameWithoutExtension(destinationFull)+".user-corrections.csv");
            using var log=new StreamWriter(logPath,false,new UTF8Encoding(false));
            log.WriteLine("action,start_sec,end_sec,original_donor,engine_reason");
            foreach(var repair in selected.OrderBy(repair=>repair.Start))
                log.WriteLine(string.Join(",",
                    "RESTORE_MASTER",
                    repair.Start.ToString("0.00000000",CultureInfo.InvariantCulture),
                    repair.End.ToString("0.00000000",CultureInfo.InvariantCulture),
                    repair.DonorSource.ToString(CultureInfo.InvariantCulture),
                    repair.DecisionReason));
            return restoredFrames;
        }
        catch
        {
            try { File.Delete(destinationFull); } catch { }
            throw;
        }
    }
}
