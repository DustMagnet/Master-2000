using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;

namespace Vinyl2000.App;

internal static class WaveformPlot
{
    internal const double DonorLaneHeight=120.0;

    internal sealed class Reader : IDisposable
    {
        private readonly FileStream _stream;
        private readonly long _dataOffset;
        private readonly long _dataBytes;
        internal readonly int SampleRate;
        internal readonly int Channels;
        internal readonly int Bits;
        private int BytesPerSample => Bits / 8;
        private int BlockAlign => Channels * BytesPerSample;

        internal Reader(string path)
        {
            _stream = File.OpenRead(path);
            using var br = new BinaryReader(_stream, System.Text.Encoding.ASCII, leaveOpen: true);
            if (new string(br.ReadChars(4)) != "RIFF") throw new InvalidDataException("Not RIFF");
            br.ReadUInt32();
            if (new string(br.ReadChars(4)) != "WAVE") throw new InvalidDataException("Not WAVE");
            int channels = 0, rate = 0, bits = 0;
            long dataOffset = 0, dataBytes = 0;
            while (_stream.Position + 8 <= _stream.Length)
            {
                string id = new string(br.ReadChars(4));
                uint size = br.ReadUInt32();
                long next = _stream.Position + size + (size & 1);
                if (id == "fmt ")
                {
                    ushort format = br.ReadUInt16();
                    channels = br.ReadUInt16();
                    rate = (int)br.ReadUInt32();
                    br.ReadUInt32(); br.ReadUInt16(); bits = br.ReadUInt16();
                    if (format != 1) throw new InvalidDataException("Only PCM WAV is supported");
                }
                else if (id == "data")
                {
                    dataOffset = _stream.Position;
                    dataBytes = size;
                }
                _stream.Position = Math.Min(next, _stream.Length);
                if (dataOffset > 0 && channels > 0) break;
            }
            if (dataOffset <= 0 || channels <= 0 || rate <= 0 || bits is not (8 or 16 or 24 or 32))
                throw new InvalidDataException("Unsupported PCM WAV");
            _dataOffset = dataOffset; _dataBytes = dataBytes;
            SampleRate = rate; Channels = channels; Bits = bits;
        }

        internal float[] ReadMono(long startFrame, int count)
        {
            var result = new float[count];
            Array.Fill(result, float.NaN);
            long totalFrames = _dataBytes / BlockAlign;
            long first = Math.Max(0, startFrame);
            long last = Math.Min(totalFrames, startFrame + count);
            if (last <= first) return result;
            int validFrames = checked((int)(last - first));
            byte[] bytes = new byte[validFrames * BlockAlign];
            _stream.Position = _dataOffset + first * BlockAlign;
            _stream.ReadExactly(bytes);
            int dst = checked((int)(first - startFrame));
            for (int i = 0; i < validFrames; ++i)
            {
                double sum = 0.0;
                for (int ch = 0; ch < Channels; ++ch)
                {
                    int o = i * BlockAlign + ch * BytesPerSample;
                    sum += Bits switch
                    {
                        8 => (bytes[o] - 128) / 128.0,
                        16 => BitConverter.ToInt16(bytes, o) / 32768.0,
                        24 => Read24(bytes, o) / 8388608.0,
                        32 => BitConverter.ToInt32(bytes, o) / 2147483648.0,
                        _ => 0.0
                    };
                }
                result[dst + i] = (float)(sum / Channels);
            }
            return result;
        }

        internal float[] ReadMonoLinear(double startFrame,int count)
        {
            if(count<=0) return [];
            long first=(long)Math.Floor(startFrame);
            double fraction=startFrame-first;
            float[] raw=ReadMono(first,count+1);
            var result=new float[count];
            for(int i=0;i<count;++i)
            {
                float a=raw[i],b=raw[i+1];
                if(!float.IsFinite(a))
                    result[i]=float.NaN;
                else if(fraction<=1e-12)
                    result[i]=a;
                else if(!float.IsFinite(b))
                    result[i]=float.NaN;
                else
                    result[i]=(float)(a+(b-a)*fraction);
            }
            return result;
        }

        private static int Read24(byte[] b, int o)
        {
            int v = b[o] | (b[o + 1] << 8) | (b[o + 2] << 16);
            return (v & 0x800000) != 0 ? v | unchecked((int)0xFF000000) : v;
        }

        public void Dispose() => _stream.Dispose();
    }

    private static readonly Color[] SourceColors =
    [
        Color.FromArgb(255, 0, 103, 192),
        Color.FromArgb(255, 190, 90, 0),
        Color.FromArgb(255, 96, 55, 170),
        Color.FromArgb(255, 174, 52, 124),
        Color.FromArgb(255, 0, 128, 138),
        Color.FromArgb(255, 112, 82, 20)
    ];
    private static readonly Color ResultColor = Color.FromArgb(255, 20, 135, 70);

    private readonly record struct DisplayMatch(double Gain,double Dc)
    {
        internal static readonly DisplayMatch Identity=new(1.0,0.0);
    }

    // Robust display-only level match. Median/MAD ignores isolated clicks and
    // keeps the actual WAV data untouched. A positive covariance check prevents
    // a misleading match when a source is not locally aligned.
    private static DisplayMatch EstimateDisplayMatch(float[] reference,float[] candidate)
    {
        int count=Math.Min(reference.Length,candidate.Length);
        if(count<32) return DisplayMatch.Identity;
        int step=Math.Max(1,count/3000);
        var a=new List<double>();var b=new List<double>();
        for(int i=0;i<count;i+=step)
            if(float.IsFinite(reference[i]) && float.IsFinite(candidate[i]))
            {a.Add(reference[i]);b.Add(candidate[i]);}
        if(a.Count<32) return DisplayMatch.Identity;
        static double Median(List<double> values)
        {
            var copy=values.ToArray();Array.Sort(copy);
            int middle=copy.Length/2;
            return copy.Length%2==0?(copy[middle-1]+copy[middle])*0.5:copy[middle];
        }
        double ma=Median(a),mb=Median(b);
        var da=a.Select(value=>Math.Abs(value-ma)).ToList();
        var db=b.Select(value=>Math.Abs(value-mb)).ToList();
        double madA=Median(da),madB=Median(db);
        if(madA<1e-8 || madB<1e-8) return DisplayMatch.Identity;
        double covariance=0.0;
        for(int i=0;i<a.Count;++i) covariance+=(a[i]-ma)*(b[i]-mb);
        if(covariance<=0.0) return DisplayMatch.Identity;
        double gain=Math.Clamp(madA/madB,0.125,8.0);
        return new DisplayMatch(gain,ma-gain*mb);
    }

    private static float[] ApplyDisplayMatch(float[] input,DisplayMatch match)
    {
        var output=new float[input.Length];
        for(int i=0;i<input.Length;++i)
            output[i]=float.IsFinite(input[i])
                ? (float)(match.Gain*input[i]+match.Dc)
                : float.NaN;
        return output;
    }

    internal static void DrawSplit(Canvas masterCanvas,Canvas donorCanvas,
        Canvas resultCanvas,IReadOnlyList<string> sourcePaths,
        IReadOnlyList<string> sourceNames,string? outputPath,
        DecisionMapData map,double centerSec,double spanSec,double outputSpeedFactor,
        bool showRepairOverlay,bool levelMatchDisplay,
        IReadOnlyDictionary<int,string> sourceRoles,
        IReadOnlySet<DecisionMapData.RepairInterval> revertedRepairs)
    {
        masterCanvas.Children.Clear();
        donorCanvas.Children.Clear();
        resultCanvas.Children.Clear();
        var paths=sourcePaths.Take(6).ToList();
        if(paths.Count==0 || string.IsNullOrWhiteSpace(outputPath) ||
           !File.Exists(outputPath)) return;
        if(masterCanvas.ActualWidth<=120 || resultCanvas.ActualWidth<=120) return;

        int masterSource=Math.Clamp(map.MasterSource,1,paths.Count);
        var donorSources=Enumerable.Range(1,paths.Count)
            .Where(source=>source!=masterSource).ToList();
        double requiredDonorHeight=Math.Max(DonorLaneHeight,donorSources.Count*DonorLaneHeight);
        if(double.IsNaN(donorCanvas.Height) ||
           Math.Abs(donorCanvas.Height-requiredDonorHeight)>0.5)
            donorCanvas.Height=requiredDonorHeight;

        var readers=new List<Reader>();
        try
        {
            foreach(string path in paths) readers.Add(new Reader(path));
            using var resultReader=new Reader(outputPath);
            int rate=readers[0].SampleRate;
            int count=Math.Max(32,(int)Math.Round(spanSec*rate));
            double leftSec=centerSec-spanSec/2.0;
            var sourceWaves=new Dictionary<int,float[]>();
            for(int source=1;source<=readers.Count;++source)
            {
                double delay=map.DelayAt(source-1,centerSec);
                long start=(long)Math.Round(leftSec*rate+delay);
                sourceWaves[source]=readers[source-1].ReadMono(start,count);
            }
            var displayMatches=sourceWaves.Keys.ToDictionary(
                source=>source,_=>DisplayMatch.Identity);
            if(levelMatchDisplay)
            {
                float[] masterWave=sourceWaves[masterSource];
                foreach(int source in sourceWaves.Keys.Where(source=>source!=masterSource).ToList())
                {
                    DisplayMatch match=EstimateDisplayMatch(masterWave,sourceWaves[source]);
                    displayMatches[source]=match;
                    sourceWaves[source]=ApplyDisplayMatch(sourceWaves[source],match);
                }
            }
            double factor=Math.Max(0.000001,outputSpeedFactor);
            long resultStart=(long)Math.Round(leftSec*rate/factor);
            int resultCount=Math.Max(1,(int)Math.Round(count/factor));
            float[] resultWave=resultReader.ReadMono(resultStart,resultCount);
            var visibleReverted=revertedRepairs
                .Where(repair=>repair.End>=leftSec && repair.Start<=leftSec+spanSec)
                .OrderBy(repair=>repair.Start).ToList();
            if(visibleReverted.Count>0)
            {
                string baselinePath=UserCorrection.MasterOnlyPath(outputPath);
                if(File.Exists(baselinePath))
                {
                    using var baselineReader=new Reader(baselinePath);
                    float[] baselineWave=baselineReader.ReadMono(resultStart,resultCount);
                    int repairIndex=0;
                    for(int sample=0;sample<resultWave.Length && repairIndex<visibleReverted.Count;++sample)
                    {
                        double capturedTime=((double)resultStart+sample)/rate*factor;
                        while(repairIndex<visibleReverted.Count &&
                              capturedTime>=visibleReverted[repairIndex].End)
                            ++repairIndex;
                        if(repairIndex<visibleReverted.Count &&
                           capturedTime>=visibleReverted[repairIndex].Start &&
                           capturedTime<visibleReverted[repairIndex].End &&
                           sample<baselineWave.Length)
                            resultWave[sample]=baselineWave[sample];
                    }
                }
            }
            double sharedPeak=Math.Max(0.00001,sourceWaves.Values
                .SelectMany(wave=>wave).Concat(resultWave)
                .Where(float.IsFinite).Select(value=>Math.Abs((double)value))
                .DefaultIfEmpty(0.00001).Max());

            string RoleFor(int source)
            {
                if(source==masterSource) return "MASTER";
                if(!sourceRoles.TryGetValue(source,out string? role)) return "SOURCE";
                return role.ToUpperInvariant() switch
                {
                    "DONOR" => "DONATOR",
                    "RESERVE" => "RESERV",
                    "REJECTED" => "AVVISAD",
                    _ => role.ToUpperInvariant()
                };
            }

            void DrawLane(Canvas canvas,float[] wave,double top,double laneHeight,
                int? source,string label,Color color,bool result)
            {
                double width=canvas.ActualWidth;
                if(width<=120) return;
                const double plotLeft=112.0;
                double plotRight=Math.Max(plotLeft+1.0,width-12.0);
                double middle=top+laneHeight*0.5;
                if(showRepairOverlay)
                    DrawSplitOverlay(canvas,map,leftSec,leftSec+spanSec,plotLeft,
                        plotRight,top,laneHeight,source,result,paths.Count,
                        revertedRepairs);
                canvas.Children.Add(new Line {
                    X1=plotLeft,X2=plotRight,Y1=middle,Y2=middle,
                    Stroke=new SolidColorBrush(Color.FromArgb(70,100,110,120)),
                    StrokeThickness=1,IsHitTestVisible=false
                });
                var laneLabel=new TextBlock {
                    Text=label,FontSize=10,
                    FontWeight=Microsoft.UI.Text.FontWeights.SemiBold,
                    Foreground=new SolidColorBrush(color),IsHitTestVisible=false,
                    MaxWidth=104,
                    TextTrimming=Microsoft.UI.Xaml.TextTrimming.CharacterEllipsis
                };
                Canvas.SetLeft(laneLabel,8);Canvas.SetTop(laneLabel,middle-16);
                canvas.Children.Add(laneLabel);

                int points=Math.Max(100,Math.Min(1800,(int)width-120));
                Polyline? poly=null;
                for(int p=0;p<points;++p)
                {
                    int a=p*wave.Length/points;
                    int b=Math.Min(wave.Length,Math.Max(a+1,(p+1)*wave.Length/points));
                    int minimum=-1,maximum=-1;
                    for(int k=a;k<b;++k)
                    {
                        if(!float.IsFinite(wave[k])) continue;
                        if(minimum<0 || wave[k]<wave[minimum]) minimum=k;
                        if(maximum<0 || wave[k]>wave[maximum]) maximum=k;
                    }
                    if(minimum<0)
                    {
                        if(poly is not null && poly.Points.Count>1) canvas.Children.Add(poly);
                        poly=null;continue;
                    }
                    poly ??= new Polyline {
                        Stroke=new SolidColorBrush(color),StrokeThickness=1.35,
                        IsHitTestVisible=false
                    };
                    // Retain positive and negative extrema in sample order.
                    // One absolute-peak sample per bucket hides the other polarity.
                    void AddSample(int sample)
                    {
                        double x=plotLeft+(plotRight-plotLeft)*sample/Math.Max(1,wave.Length-1);
                        double y=middle-wave[sample]/sharedPeak*laneHeight*0.425;
                        poly!.Points.Add(new Point(x,y));
                    }
                    AddSample(Math.Min(minimum,maximum));
                    if(minimum!=maximum) AddSample(Math.Max(minimum,maximum));
                }
                if(poly is not null && poly.Points.Count>1) canvas.Children.Add(poly);
                double markerX=plotLeft+(plotRight-plotLeft)*0.5;
                canvas.Children.Add(new Line {
                    X1=markerX,X2=markerX,Y1=top,Y2=top+laneHeight,
                    Stroke=new SolidColorBrush(Color.FromArgb(220,190,35,35)),
                    StrokeThickness=1.5,IsHitTestVisible=false
                });
            }

            void DrawRenderedDonorCores(double laneHeight)
            {
                if(!showRepairOverlay) return;
                double width=resultCanvas.ActualWidth;
                if(width<=120) return;
                const double plotLeft=112.0;
                double plotRight=Math.Max(plotLeft+1.0,width-12.0);
                double middle=laneHeight*0.5;
                double span=Math.Max(0.000001,spanSec);
                double X(double sec)=>plotLeft+(plotRight-plotLeft)*(sec-leftSec)/span;
                foreach(var repair in map.RepairIntervals(leftSec,leftSec+spanSec))
                {
                    if(!repair.HasExactCoreTrace || revertedRepairs.Contains(repair) ||
                       repair.DonorSource<1 || repair.DonorSource>readers.Count) continue;
                    // Colour the samples which are actually present in Result.
                    // Recomputing a second donor trace can drift vertically or
                    // horizontally after output-speed resampling and falsely
                    // look like a different curve.
                    var points=new List<Point>();
                    for(int i=0;i<resultWave.Length;++i)
                    {
                        if(!float.IsFinite(resultWave[i])) continue;
                        double capturedTime=((double)resultStart+i)/rate*factor;
                        if(capturedTime<repair.CoreStart ||
                           capturedTime>=repair.CoreEnd) continue;
                        points.Add(new Point(X(capturedTime),
                            middle-resultWave[i]/sharedPeak*laneHeight*0.425));
                    }
                    if(points.Count<2) continue;
                    Color donorColor=SourceColors[Math.Min(
                        repair.DonorSource-1,SourceColors.Length-1)];
                    var trace=new Polyline {
                        Stroke=new SolidColorBrush(donorColor),
                        StrokeThickness=2.4,IsHitTestVisible=false
                    };
                    foreach(Point point in points) trace.Points.Add(point);
                    resultCanvas.Children.Add(trace);
                }
            }

            Color masterColor=SourceColors[Math.Min(masterSource-1,SourceColors.Length-1)];
            string DisplayName(int source)=>source<=sourceNames.Count
                ? sourceNames[source-1] : System.IO.Path.GetFileName(paths[source-1]);
            DrawLane(masterCanvas,sourceWaves[masterSource],0,
                Math.Max(60,masterCanvas.ActualHeight),masterSource,
                $"S{masterSource} {DisplayName(masterSource)}\nMASTER",masterColor,false);
            for(int lane=0;lane<donorSources.Count;++lane)
            {
                int source=donorSources[lane];
                string role=RoleFor(source);
                DisplayMatch match=displayMatches[source];
                string levelNote=levelMatchDisplay
                    ? $"\nMATCH ×{match.Gain:0.###} ({20.0*Math.Log10(Math.Max(1e-9,match.Gain)):+0.0;-0.0;0.0} dB)"
                    : "\nRAW LEVEL";
                Color color=role=="AVVISAD"
                    ? Color.FromArgb(255,125,130,136)
                    : SourceColors[Math.Min(source-1,SourceColors.Length-1)];
                DrawLane(donorCanvas,sourceWaves[source],lane*DonorLaneHeight,
                    DonorLaneHeight,source,$"S{source} {DisplayName(source)}\n{role}{levelNote}",color,false);
            }
            double resultLaneHeight=Math.Max(60,resultCanvas.ActualHeight-22.0);
            DrawLane(resultCanvas,resultWave,0,resultLaneHeight,null,"Result",
                ResultColor,true);
            DrawRenderedDonorCores(resultLaneHeight);
            double resultWidth=resultCanvas.ActualWidth;
            const double timeLeft=112.0;
            double timeRight=Math.Max(timeLeft+1.0,resultWidth-12.0);
            double resultMarkerX=timeLeft+(timeRight-timeLeft)*0.5;
            resultCanvas.Children.Add(new Line {
                X1=resultMarkerX,X2=resultMarkerX,Y1=0,Y2=resultLaneHeight,
                Stroke=new SolidColorBrush(Color.FromArgb(220,190,35,35)),
                StrokeThickness=1.5,IsHitTestVisible=false
            });
            AddTimeLabel(resultCanvas,timeLeft,resultCanvas.ActualHeight-18,
                Math.Max(0.0,leftSec));
            AddTimeLabel(resultCanvas,(timeLeft+timeRight)*0.5-25,
                resultCanvas.ActualHeight-18,centerSec);
            AddTimeLabel(resultCanvas,timeRight-50,resultCanvas.ActualHeight-18,
                leftSec+spanSec);
            if(showRepairOverlay)
                AddOverlayLabel(masterCanvas,map,centerSec,
                    Math.Max(112.0,masterCanvas.ActualWidth-12.0),paths.Count,
                    revertedRepairs);
        }
        catch(Exception ex)
        {
            var error=new TextBlock {Text="Waveform unavailable: "+ex.Message,
                Foreground=new SolidColorBrush(Color.FromArgb(255,150,25,25))};
            Canvas.SetLeft(error,12);Canvas.SetTop(error,12);
            masterCanvas.Children.Add(error);
        }
        finally
        {
            foreach(var reader in readers) reader.Dispose();
        }
    }

    private static void DrawSplitOverlay(Canvas canvas,DecisionMapData map,
        double leftSec,double rightSec,double plotLeft,double plotRight,
        double laneTop,double laneHeight,int? source,bool result,int sourceCount,
        IReadOnlySet<DecisionMapData.RepairInterval> revertedRepairs)
    {
        double span=Math.Max(0.000001,rightSec-leftSec);
        double X(double sec)=>plotLeft+(plotRight-plotLeft)*(sec-leftSec)/span;
        void AddBand(double start,double end,Color color)
        {
            double x1=Math.Max(plotLeft,X(start));
            double x2=Math.Min(plotRight,X(end));
            if(x2<plotLeft || x1>plotRight) return;
            var band=new Rectangle {
                Width=Math.Max(2.0,x2-x1),Height=Math.Max(2.0,laneHeight-8.0),
                Fill=new SolidColorBrush(color),IsHitTestVisible=false
            };
            Canvas.SetLeft(band,x1);Canvas.SetTop(band,laneTop+4.0);
            canvas.Children.Add(band);
        }
        void AddRibbon(double start,double end,Color color)
        {
            double x1=Math.Max(plotLeft,X(start));
            double x2=Math.Min(plotRight,X(end));
            if(x2<plotLeft || x1>plotRight) return;
            var ribbon=new Rectangle {
                Width=Math.Max(2.0,x2-x1),Height=4.0,
                Fill=new SolidColorBrush(color),IsHitTestVisible=false
            };
            Canvas.SetLeft(ribbon,x1);
            Canvas.SetTop(ribbon,laneTop+laneHeight-7.0);
            canvas.Children.Add(ribbon);
        }
        void DrawAccepted(double start,double end,int donor,
            double coreStart=double.NaN,double coreEnd=double.NaN)
        {
            bool detailed=double.IsFinite(coreStart) && double.IsFinite(coreEnd) &&
                coreStart>=start && coreEnd>coreStart && coreEnd<=end;
            if(result)
            {
                AddBand(start,end,Color.FromArgb(34,20,170,75));
                if(detailed) AddBand(coreStart,coreEnd,Color.FromArgb(88,20,170,75));
            }
            else if(source==map.MasterSource)
            {
                AddBand(start,end,Color.FromArgb(58,220,35,35));
                if(detailed) AddBand(coreStart,coreEnd,Color.FromArgb(96,220,35,35));
            }
            else if(source==donor)
            {
                AddBand(start,end,Color.FromArgb(44,20,170,75));
                if(detailed) AddBand(coreStart,coreEnd,Color.FromArgb(104,20,170,75));
            }
            else if(source.HasValue)
            {
                var supporters=map.SupportingSourcesAt((start+end)*0.5).ToList();
                if(supporters.Count==0)
                {
                    int legacy=ConfirmingSource(map.MasterSource,donor,sourceCount);
                    if(legacy>0) supporters.Add(legacy);
                }
                if(supporters.Contains(source.Value))
                {
                    AddBand(start,end,Color.FromArgb(72,0,145,170));
                    if(detailed) AddBand(coreStart,coreEnd,Color.FromArgb(88,0,145,170));
                }
            }
        }
        // Draw analysis-only B regions first. A final accepted repair is then
        // painted over them so amber can never hide the actual rendered span.
        double centerSec=0.5*(leftSec+rightSec);
        foreach(var interval in map.FlaggedIntervals(leftSec,rightSec))
            if(result || source==map.MasterSource)
            {
                bool selected=centerSec>=interval.Start && centerSec<interval.End;
                if(selected)
                    AddBand(interval.Start,interval.End,result
                        ? Color.FromArgb(45,230,165,20)
                        : Color.FromArgb(72,230,165,20));
                else
                    AddRibbon(interval.Start,interval.End,
                        Color.FromArgb(90,205,135,0));
            }
        var repairs=map.RepairIntervals(leftSec,rightSec).ToList();
        if(map.HasRepairLog)
            foreach(var repair in repairs)
                if(revertedRepairs.Contains(repair))
                {
                    if(result || source==map.MasterSource)
                        AddBand(repair.Start,repair.End,
                            Color.FromArgb(result?(byte)48:(byte)76,95,80,180));
                }
                else DrawAccepted(repair.Start,repair.End,repair.DonorSource,
                    repair.CoreStart,repair.CoreEnd);
        else // Backward compatibility for maps created before repair logs.
            foreach(var interval in map.DecisionIntervals(leftSec,rightSec).Where(i=>i.Class=='A'))
                DrawAccepted(interval.Start,interval.End,interval.SelectedSource);
    }

    internal static void Draw(Canvas canvas, IReadOnlyList<string> sourcePaths,
        string? outputPath, DecisionMapData map, double centerSec, double spanSec,
        double outputSpeedFactor, bool showRepairOverlay)
    {
        canvas.Children.Clear();
        double w = canvas.ActualWidth;
        double h = canvas.ActualHeight;
        // Wait for layout; never draw a larger virtual canvas than is visible.
        if (!double.IsFinite(w) || !double.IsFinite(h) || w <= 100.0 || h <= 48.0) return;
        const double timeAxisHeight = 24.0;
        double plotHeight = h - timeAxisHeight;
        var paths = sourcePaths.Take(6).ToList();
        if (!string.IsNullOrWhiteSpace(outputPath) && File.Exists(outputPath)) paths.Add(outputPath);
        if (paths.Count == 0) return;

        var readers = new List<Reader>();
        try
        {
            foreach (string path in paths) readers.Add(new Reader(path));
            int rate = readers[0].SampleRate;
            int count = Math.Max(32, (int)Math.Round(spanSec * rate));
            int points = Math.Max(100, Math.Min(1600, (int)w - 100));
            double leftSec = centerSec - spanSec / 2.0;
            var waves = new List<float[]>();
            for (int i = 0; i < readers.Count; ++i)
            {
                bool isResult=i>=sourcePaths.Count;
                double delay = !isResult ? map.DelayAt(i, centerSec) : 0.0;
                double factor=isResult ? Math.Max(0.000001,outputSpeedFactor) : 1.0;
                long start = (long)Math.Round(leftSec * rate / factor + delay);
                int laneCount=isResult ? Math.Max(1,(int)Math.Round(count/factor)) : count;
                waves.Add(readers[i].ReadMono(start,laneCount));
            }

            var finiteSamples = waves.SelectMany(x => x).Where(float.IsFinite);
            double sharedPeak = Math.Max(0.00001,
                finiteSamples.Any() ? finiteSamples.Max(x => Math.Abs((double)x)) : 0.00001);
            double plotLeft = 78.0, plotRight = w - 12.0;
            double laneH = plotHeight / waves.Count;
            if (showRepairOverlay)
                DrawRepairOverlay(canvas, map, leftSec, leftSec + spanSec, plotLeft, plotRight, laneH, waves.Count);
            for (int lane = 0; lane < waves.Count; ++lane)
            {
                double mid = laneH * (lane + 0.5);
                var baseline = new Line { X1 = plotLeft, X2 = plotRight, Y1 = mid, Y2 = mid,
                    Stroke = new SolidColorBrush(Color.FromArgb(70, 100, 110, 120)), StrokeThickness = 1 };
                canvas.Children.Add(baseline);
                bool isMasterLane=lane<sourcePaths.Count && lane+1==map.MasterSource;
                string laneLabel=lane<sourcePaths.Count
                    ? (isMasterLane ? $"Source {lane + 1}\nMASTER" : $"Source {lane + 1}")
                    : "Result";
                Color laneColor=lane<sourcePaths.Count
                    ? SourceColors[Math.Min(lane,SourceColors.Length-1)]
                    : ResultColor;
                var label = new TextBlock { Text = laneLabel,
                    FontSize = isMasterLane ? 10 : 12,
                    FontWeight = isMasterLane ? Microsoft.UI.Text.FontWeights.SemiBold : Microsoft.UI.Text.FontWeights.Normal,
                    Foreground = new SolidColorBrush(laneColor) };
                Canvas.SetLeft(label, 8); Canvas.SetTop(label, mid - (isMasterLane ? 17 : 10)); canvas.Children.Add(label);

                Polyline? poly = null;
                float[] wave = waves[lane];
                for (int p = 0; p < points; ++p)
                {
                    int a = p * wave.Length / points;
                    int b = Math.Max(a + 1, (p + 1) * wave.Length / points);
                    b = Math.Min(b, wave.Length);
                    int pick = a;
                    double peak = -1.0;
                    for (int k = a; k < b; ++k)
                    {
                        if (!float.IsFinite(wave[k])) continue;
                        double av = Math.Abs((double)wave[k]);
                        if (av > peak) { peak = av; pick = k; }
                    }
                    if (peak < 0.0)
                    {
                        if (poly is not null && poly.Points.Count > 1) canvas.Children.Add(poly);
                        poly = null;
                        continue;
                    }
                    poly ??= new Polyline
                    {
                        Stroke = new SolidColorBrush(laneColor),
                        StrokeThickness = 1.25
                    };
                    double x = plotLeft + (plotRight - plotLeft) * p / Math.Max(1, points - 1);
                    // Use one shared display scale for every source and Result.
                    // scale. Relative source levels and all WAV samples remain
                    // unchanged, while every trace stays inside its own lane.
                    double y = mid - wave[pick] / sharedPeak * laneH * 0.43;
                    poly.Points.Add(new Point(x, y));
                }
                if (poly is not null && poly.Points.Count > 1) canvas.Children.Add(poly);
            }

            double markerX = plotLeft + (plotRight - plotLeft) * 0.5;
            canvas.Children.Add(new Line { X1 = markerX, X2 = markerX, Y1 = 0, Y2 = h,
                Stroke = new SolidColorBrush(Color.FromArgb(220, 190, 35, 35)), StrokeThickness = 1.5 });
            AddTimeLabel(canvas, plotLeft, h - 18, Math.Max(0.0, leftSec));
            AddTimeLabel(canvas, markerX - 25, h - 18, centerSec);
            AddTimeLabel(canvas, plotRight - 50, h - 18, leftSec + spanSec);
            if (showRepairOverlay) AddOverlayLabel(canvas,map,centerSec,plotRight,
                sourcePaths.Count,new HashSet<DecisionMapData.RepairInterval>());
        }
        catch (Exception ex)
        {
            var error = new TextBlock { Text = "Waveform unavailable: " + ex.Message, Foreground = new SolidColorBrush(Color.FromArgb(255, 150, 25, 25)) };
            Canvas.SetLeft(error, 12); Canvas.SetTop(error, 12); canvas.Children.Add(error);
        }
        finally
        {
            foreach (var reader in readers) reader.Dispose();
        }
    }

    private static void DrawRepairOverlay(Canvas canvas, DecisionMapData map,
        double leftSec, double rightSec, double plotLeft, double plotRight,
        double laneH, int laneCount)
    {
        double span = Math.Max(0.000001, rightSec - leftSec);
        double X(double sec) => plotLeft + (plotRight - plotLeft) * (sec - leftSec) / span;
        int sourceLaneMax = Math.Max(0, laneCount - 2);
        int sourceCount = Math.Max(0, laneCount - 1);
        int masterLane = Math.Clamp(map.MasterSource - 1, 0, sourceLaneMax);
        int resultLane = laneCount - 1;

        void AddBand(int lane, double start, double end, Color color)
        {
            double x1 = Math.Max(plotLeft, X(start));
            double x2 = Math.Min(plotRight, X(end));
            if (x2 < plotLeft || x1 > plotRight) return;
            var band = new Rectangle
            {
                Width = Math.Max(2.0, x2 - x1),
                Height = Math.Max(2.0, laneH - 8.0),
                Fill = new SolidColorBrush(color),
                IsHitTestVisible = false
            };
            Canvas.SetLeft(band, x1);
            Canvas.SetTop(band, lane * laneH + 4.0);
            canvas.Children.Add(band);
        }

        void DrawAccepted(double start,double end,int donor,
            double coreStart=double.NaN,double coreEnd=double.NaN)
        {
            bool detailed=double.IsFinite(coreStart) && double.IsFinite(coreEnd) &&
                coreStart>=start && coreEnd>coreStart && coreEnd<=end;
            AddBand(masterLane,start,end,Color.FromArgb(58,220,35,35));
            if(detailed) AddBand(masterLane,coreStart,coreEnd,Color.FromArgb(96,220,35,35));
            int donorLane=Math.Clamp(donor-1,0,sourceLaneMax);
            AddBand(donorLane,start,end,Color.FromArgb(44,20,170,75));
            if(detailed) AddBand(donorLane,coreStart,coreEnd,Color.FromArgb(104,20,170,75));
            var confirmingSources=map.SupportingSourcesAt((start+end)*0.5)
                .Where(source=>source!=map.MasterSource && source!=donor)
                .Distinct().ToList();
            if(confirmingSources.Count==0)
            {
                int legacy=ConfirmingSource(map.MasterSource,donor,sourceCount);
                if(legacy>0) confirmingSources.Add(legacy);
            }
            foreach(int confirmingSource in confirmingSources)
            {
                AddBand(Math.Clamp(confirmingSource-1,0,sourceLaneMax),
                    start,end,Color.FromArgb(72,0,145,170));
                if(detailed) AddBand(Math.Clamp(confirmingSource-1,0,sourceLaneMax),
                    coreStart,coreEnd,Color.FromArgb(88,0,145,170));
            }
            AddBand(resultLane,start,end,Color.FromArgb(34,20,170,75));
            if(detailed) AddBand(resultLane,coreStart,coreEnd,Color.FromArgb(88,20,170,75));
        }
        foreach(var interval in map.DecisionIntervals(leftSec,rightSec))
            if(interval.Class=='B')
            {
                AddBand(masterLane, interval.Start, interval.End, Color.FromArgb(72, 230, 165, 20));
                AddBand(resultLane, interval.Start, interval.End, Color.FromArgb(45, 230, 165, 20));
            }
        var repairs=map.RepairIntervals(leftSec,rightSec).ToList();
        if(map.HasRepairLog)
            foreach(var repair in repairs)
                DrawAccepted(repair.Start,repair.End,repair.DonorSource,
                    repair.CoreStart,repair.CoreEnd);
        else
            foreach(var interval in map.DecisionIntervals(leftSec,rightSec).Where(i=>i.Class=='A'))
                DrawAccepted(interval.Start,interval.End,interval.SelectedSource);
    }

    private static void AddOverlayLabel(Canvas canvas, DecisionMapData map,
        double centerSec,double plotRight,int sourceCount,
        IReadOnlySet<DecisionMapData.RepairInterval> revertedRepairs)
    {
        var repair=map.RepairIntervalAt(centerSec);
        var interval = map.DecisionIntervalAt(centerSec);
        bool userReverted=repair is { } possibleRepair &&
            revertedRepairs.Contains(possibleRepair);
        string text;
        Color color;
        if(map.HasRepairLog && repair is null)
        {
            bool flagged=interval is { } currentDecision && currentDecision.Class=='B';
            text=flagged
                ? "Flagged: Master retained • no donor inserted"
                : "At cursor: Master unchanged • no donor inserted";
            color=flagged
                ? Color.FromArgb(255,170,105,0)
                : Color.FromArgb(255,65,90,120);
        }
        else
        {
            int selectedSource;
            bool accepted;
            if(repair is { } finalRepair)
            {
                selectedSource=userReverted?map.MasterSource:finalRepair.DonorSource;
                accepted=!userReverted;
            }
            else if(interval is { } decision)
            {
                selectedSource=decision.SelectedSource;
                accepted=decision.Class=='A';
            }
            else return;
            var confirmingSources=map.SupportingSourcesAt(centerSec)
                .Where(source=>source!=map.MasterSource && source!=selectedSource)
                .Distinct()
                .ToList();
            if(confirmingSources.Count==0)
            {
                int legacy=ConfirmingSource(map.MasterSource,selectedSource,sourceCount);
                if(legacy>0) confirmingSources.Add(legacy);
            }
            string confirmation=confirmingSources.Count>0
                ? $" • Sources {string.Join(" + ",confirmingSources)} confirmed"
                : "";
            if(userReverted)
                text="User undo: exact Master restored";
            else if(repair is { } renderedRepair && renderedRepair.HasRenderDetails)
                text=renderedRepair.IsCoreAt(centerSec)
                    ? $"Source {selectedSource} rendered core ×{renderedRepair.RenderGain:0.###}{confirmation}"
                    : $"Crossfade: Master ↔ Source {selectedSource}{confirmation}";
            else if(accepted)
                text=$"Source {selectedSource} inserted{confirmation}";
            else
                text="Flagged: Master retained • no donor inserted";
            color=userReverted
                ? Color.FromArgb(255,95,80,180)
                : accepted
                ? Color.FromArgb(255,15,125,55)
                : Color.FromArgb(255,170,105,0);
        }
        var label = new TextBlock
        {
            Text = text,
            FontSize = 11,
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
            Foreground = new SolidColorBrush(color),
            IsHitTestVisible = false
        };
        Canvas.SetLeft(label, Math.Max(82.0, plotRight - 320.0));
        Canvas.SetTop(label, 5.0);
        canvas.Children.Add(label);
    }

    private static int ConfirmingSource(int masterSource, int donorSource, int sourceCount)
    {
        // Backward compatibility for x2k and older three-source maps, which did
        // not store the explicit supporting-source list.
        if (sourceCount != 3 || donorSource == masterSource) return 0;
        for (int source = 1; source <= sourceCount; ++source)
            if (source != masterSource && source != donorSource) return source;
        return 0;
    }

    private static void AddTimeLabel(Canvas canvas, double x, double y, double sec)
    {
        var t = new TextBlock { Text = $"{sec:0.000000} s", FontSize = 11,
            Foreground = new SolidColorBrush(Color.FromArgb(255, 80, 85, 90)) };
        Canvas.SetLeft(t, x); Canvas.SetTop(t, y); canvas.Children.Add(t);
    }
}
