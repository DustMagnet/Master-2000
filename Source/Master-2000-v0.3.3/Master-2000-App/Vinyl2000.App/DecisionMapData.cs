using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Microsoft.UI.Xaml;
using Windows.Foundation;

namespace Vinyl2000.App;

internal sealed class DecisionMapData
{
    internal readonly List<double> TimeSec = [];
    internal readonly List<int> SelectedSource = [];
    internal readonly List<char> ConfidenceClass = [];
    internal readonly List<string> DecisionReasons = [];
    internal readonly List<double[]> Delays = [];
    internal readonly List<int[]> SupportingSources = [];
    internal readonly List<int[]> AbstainingSources = [];
    internal readonly List<int[]> OpposingSources = [];
    internal readonly List<RepairInterval> Repairs = [];
    internal bool HasRepairLog { get; private set; }
    private List<Run>? _cachedRuns;
    private double _cachedRunDuration;
    private int? _cachedMasterSource;

    internal readonly record struct DecisionInterval(double Start, double End, char Class, int SelectedSource);
    internal readonly record struct RepairInterval(double Start, double End, int DonorSource,
        double OutlierRatio, double MaxAbsDifference, int RetryAttempt=0,
        double DelayNudgeSamples=0.0, string DecisionReason="A_CLICK_REPAIR",
        double CoreStart=double.NaN, double CoreEnd=double.NaN,
        int FadeInSamples=0, int FadeOutSamples=0,
        double RenderGain=double.NaN, double RenderDc=double.NaN,
        double DonorDelaySamples=double.NaN)
    {
        internal bool HasRenderDetails =>
            double.IsFinite(CoreStart) && double.IsFinite(CoreEnd) &&
            CoreStart>=Start && CoreEnd>CoreStart && CoreEnd<=End &&
            double.IsFinite(RenderGain) && double.IsFinite(RenderDc);

        // A localized sub-ms repair uses one fixed, logged donor delay for the
        // complete full-donor core.  A broad repair can use a different delay
        // in every decision cell, so one logged midpoint delay must not be
        // presented as an exact donor trace for that case.
        internal bool HasExactCoreTrace =>
            HasRenderDetails && double.IsFinite(DonorDelaySamples) &&
            DecisionReason.Contains("SUBMS_CORE",StringComparison.OrdinalIgnoreCase);

        internal bool IsCoreAt(double timeSec) =>
            HasRenderDetails && timeSec>=CoreStart && timeSec<CoreEnd;
    }

    internal static DecisionMapData Load(string path)
    {
        var d = new DecisionMapData();
        using var r = new StreamReader(path);
        string? headerLine = r.ReadLine();
        if (headerLine is null) return d;

        string[] headers = headerLine.Split(',');
        int timeCol = Array.FindIndex(headers, h => h.Trim().Equals("time_sec", StringComparison.OrdinalIgnoreCase));
        int sourceCol = Array.FindIndex(headers, h => h.Trim().Equals("selected_source", StringComparison.OrdinalIgnoreCase));
        int classCol = Array.FindIndex(headers, h => h.Trim().Equals("confidence_class", StringComparison.OrdinalIgnoreCase));
        int reasonCol = Array.FindIndex(headers, h => h.Trim().Equals("decision_reason", StringComparison.OrdinalIgnoreCase));
        int supportingCol = Array.FindIndex(headers, h => h.Trim().Equals("supporting_sources", StringComparison.OrdinalIgnoreCase));
        int abstainingCol = Array.FindIndex(headers, h => h.Trim().Equals("abstaining_sources", StringComparison.OrdinalIgnoreCase));
        int opposingCol = Array.FindIndex(headers, h => h.Trim().Equals("opposing_sources", StringComparison.OrdinalIgnoreCase));
        var delayCols = headers
            .Select((h, i) => (Header: h.Trim(), Index: i))
            .Where(x => x.Header.StartsWith("delay_", StringComparison.OrdinalIgnoreCase))
            .OrderBy(x => x.Header, StringComparer.OrdinalIgnoreCase)
            .Select(x => x.Index)
            .ToArray();
        if (timeCol < 0 || sourceCol < 0) { d.LoadRepairIntervals(path); return d; }

        string? line;
        while ((line = r.ReadLine()) is not null)
        {
            var p = line.Split(',');
            if (p.Length <= Math.Max(timeCol, sourceCol)) continue;

            if (double.TryParse(p[timeCol], System.Globalization.NumberStyles.Float,
                    System.Globalization.CultureInfo.InvariantCulture, out double t) &&
                int.TryParse(p[sourceCol], System.Globalization.NumberStyles.Integer,
                    System.Globalization.CultureInfo.InvariantCulture, out int s))
            {
                d.TimeSec.Add(t);
                d.SelectedSource.Add(Math.Max(1, s));
                char cc = 'C';
                if (classCol >= 0 && classCol < p.Length && !string.IsNullOrWhiteSpace(p[classCol]))
                    cc = char.ToUpperInvariant(p[classCol].Trim()[0]);
                d.ConfidenceClass.Add(cc);
                d.DecisionReasons.Add(reasonCol >= 0 && reasonCol < p.Length
                    ? p[reasonCol].Trim() : "");
                var delays = new double[delayCols.Length];
                for (int i = 0; i < delayCols.Length; ++i)
                {
                    int col = delayCols[i];
                    if (col < p.Length)
                        double.TryParse(p[col], System.Globalization.NumberStyles.Float,
                            System.Globalization.CultureInfo.InvariantCulture, out delays[i]);
                }
                d.Delays.Add(delays);
                d.SupportingSources.Add(ParseSourceList(p, supportingCol));
                d.AbstainingSources.Add(ParseSourceList(p, abstainingCol));
                d.OpposingSources.Add(ParseSourceList(p, opposingCol));
            }
        }
        d.LoadRepairIntervals(path);
        return d;
    }

    private void LoadRepairIntervals(string decisionPath)
    {
        string suffix=".decisions.csv";
        string repairPath=decisionPath.EndsWith(suffix,StringComparison.OrdinalIgnoreCase)
            ? decisionPath[..^suffix.Length]+".repairs.csv"
            : decisionPath+".repairs.csv";
        if(!File.Exists(repairPath)) return;
        HasRepairLog=true;
        using var reader=new StreamReader(repairPath);
        string? headerLine=reader.ReadLine();
        if(headerLine is null) return;
        string[] headers=headerLine.Split(',').Select(header=>header.Trim()).ToArray();
        int Column(string name,int legacyIndex=-1)
        {
            int found=Array.FindIndex(headers,header=>
                header.Equals(name,StringComparison.OrdinalIgnoreCase));
            return found>=0 ? found : legacyIndex;
        }
        int startCol=Column("start_sec",0),endCol=Column("end_sec",1);
        int donorCol=Column("donor_source",3),ratioCol=Column("outlier_ratio",4);
        int differenceCol=Column("max_abs_difference",5),retryCol=Column("retry_attempt",6);
        int nudgeCol=Column("delay_nudge_samples",7),reasonCol=Column("decision_reason",8);
        int coreStartCol=Column("core_start_sec"),coreEndCol=Column("core_end_sec");
        int fadeInCol=Column("fade_in_samples"),fadeOutCol=Column("fade_out_samples");
        int gainCol=Column("render_gain"),dcCol=Column("render_dc");
        int donorDelayCol=Column("donor_delay_samples");

        static string Field(string[] fields,int column,string fallback="") =>
            column>=0 && column<fields.Length ? fields[column] : fallback;
        static double DoubleField(string[] fields,int column,double fallback=double.NaN) =>
            double.TryParse(Field(fields,column),System.Globalization.NumberStyles.Float,
                System.Globalization.CultureInfo.InvariantCulture,out double value)
                ? value : fallback;
        static int IntField(string[] fields,int column,int fallback=0) =>
            int.TryParse(Field(fields,column),System.Globalization.NumberStyles.Integer,
                System.Globalization.CultureInfo.InvariantCulture,out int value)
                ? value : fallback;

        string? line;
        while((line=reader.ReadLine()) is not null)
        {
            string[] p=line.Split(',');
            if(p.Length<4) continue;
            double start=DoubleField(p,startCol),end=DoubleField(p,endCol);
            int donor=IntField(p,donorCol);
            double ratio=DoubleField(p,ratioCol,0.0);
            double difference=DoubleField(p,differenceCol,0.0);
            int retryAttempt=IntField(p,retryCol);
            double delayNudge=DoubleField(p,nudgeCol,0.0);
            string reason=Field(p,reasonCol,"A_CLICK_REPAIR").Trim();
            double coreStart=DoubleField(p,coreStartCol);
            double coreEnd=DoubleField(p,coreEndCol);
            int fadeIn=IntField(p,fadeInCol),fadeOut=IntField(p,fadeOutCol);
            double gain=DoubleField(p,gainCol),dc=DoubleField(p,dcCol);
            double donorDelay=DoubleField(p,donorDelayCol);
            if(double.IsFinite(start) && double.IsFinite(end) && end>start && donor>0)
                Repairs.Add(new RepairInterval(start,end,donor,ratio,difference,
                    retryAttempt,delayNudge,reason,coreStart,coreEnd,
                    fadeIn,fadeOut,gain,dc,donorDelay));
        }
        Repairs.Sort((a,b)=>a.Start.CompareTo(b.Start));
    }

    internal RepairInterval? RepairIntervalAt(double timeSec)
    {
        foreach(var repair in Repairs)
            if(timeSec>=repair.Start && timeSec<repair.End) return repair;
        return null;
    }

    internal IEnumerable<RepairInterval> RepairIntervals(double startSec,double endSec) =>
        Repairs.Where(repair=>repair.End>=startSec && repair.Start<=endSec);

    private static int[] ParseSourceList(string[] fields, int column)
    {
        if (column < 0 || column >= fields.Length || string.IsNullOrWhiteSpace(fields[column]))
            return [];
        return fields[column].Split('|', StringSplitOptions.RemoveEmptyEntries)
            .Select(text => int.TryParse(text, out int source) ? source : 0)
            .Where(source => source > 0)
            .Distinct()
            .ToArray();
    }


    internal void AppendLive(double timeSec, int source)
    {
        if (!double.IsFinite(timeSec) || timeSec < 0) return;
        source = Math.Max(1, source);
        if (TimeSec.Count > 0 && timeSec < TimeSec[^1]) return;
        TimeSec.Add(timeSec);
        SelectedSource.Add(source);
        ConfidenceClass.Add(source == 1 ? 'C' : 'A');
        DecisionReasons.Add("");
        Delays.Add([]);
        SupportingSources.Add([]);
        AbstainingSources.Add([]);
        OpposingSources.Add([]);
        _cachedRuns = null;
        _cachedMasterSource = null;
    }

    internal int NearestIndex(double timeSec)
    {
        if (TimeSec.Count == 0) return -1;
        int i = TimeSec.BinarySearch(timeSec);
        if (i >= 0) return i;
        i = ~i;
        if (i <= 0) return 0;
        if (i >= TimeSec.Count) return TimeSec.Count - 1;
        return Math.Abs(TimeSec[i] - timeSec) < Math.Abs(TimeSec[i - 1] - timeSec) ? i : i - 1;
    }

    internal int ContainingIndex(double timeSec)
    {
        if (TimeSec.Count == 0 || !double.IsFinite(timeSec) || timeSec < TimeSec[0] || timeSec >= DurationSec) return -1;
        int i = TimeSec.BinarySearch(timeSec);
        return i >= 0 ? i : (~i) - 1;
    }

    internal double DelayAt(int sourceIndex, double timeSec)
    {
        int i = ContainingIndex(timeSec);
        if (i < 0 || i >= Delays.Count || sourceIndex < 0 || sourceIndex >= Delays[i].Length) return 0.0;
        return Delays[i][sourceIndex];
    }

    internal IReadOnlyList<int> SupportingSourcesAt(double timeSec)
    {
        int i = ContainingIndex(timeSec);
        return i >= 0 && i < SupportingSources.Count ? SupportingSources[i] : Array.Empty<int>();
    }

    internal IReadOnlyList<int> AbstainingSourcesAt(double timeSec)
    {
        int i = ContainingIndex(timeSec);
        return i >= 0 && i < AbstainingSources.Count ? AbstainingSources[i] : Array.Empty<int>();
    }

    internal string DecisionReasonAt(double timeSec)
    {
        int i=ContainingIndex(timeSec);
        return i>=0 && i<DecisionReasons.Count ? DecisionReasons[i] : "";
    }

    internal char ConfidenceAt(double timeSec)
    {
        int i=ContainingIndex(timeSec);
        return i>=0 && i<ConfidenceClass.Count ? ConfidenceClass[i] : 'C';
    }

    internal int MasterSource
    {
        get
        {
            if (_cachedMasterSource is int cached) return cached;
            var counts = new Dictionary<int, int>();
            for (int i = 0; i < SelectedSource.Count; ++i)
            {
                char cc = i < ConfidenceClass.Count ? ConfidenceClass[i] : 'C';
                if (cc == 'A') continue;
                int source = SelectedSource[i];
                counts[source] = counts.TryGetValue(source, out int n) ? n + 1 : 1;
            }
            _cachedMasterSource = counts.Count == 0
                ? (SelectedSource.Count == 0 ? 1 : SelectedSource[0])
                : counts.MaxBy(pair => pair.Value).Key;
            return _cachedMasterSource.Value;
        }
    }

    private double DecisionEnd(int index)
    {
        if (index + 1 < TimeSec.Count) return TimeSec[index + 1];
        double step = TimeSec.Count > 1 ? Math.Max(0.000001, TimeSec[^1] - TimeSec[^2]) : 0.002;
        return TimeSec[index] + step;
    }

    internal DecisionInterval? DecisionIntervalAt(double timeSec)
    {
        int i = ContainingIndex(timeSec);
        if (i < 0 || i >= ConfidenceClass.Count) return null;
        char cc = ConfidenceClass[i];
        if (cc is not ('A' or 'B')) return null;
        int source = SelectedSource[i];
        int first = i;
        int last = i;
        while (first > 0 && ConfidenceClass[first - 1] == cc && SelectedSource[first - 1] == source) --first;
        while (last + 1 < TimeSec.Count && ConfidenceClass[last + 1] == cc && SelectedSource[last + 1] == source) ++last;
        return new DecisionInterval(TimeSec[first], DecisionEnd(last), cc, source);
    }

    internal List<DecisionInterval> DecisionIntervals(double startSec, double endSec)
    {
        var result = new List<DecisionInterval>();
        if (TimeSec.Count == 0 || ConfidenceClass.Count != TimeSec.Count) return result;
        int i = Math.Max(0, NearestIndex(startSec) - 1);
        while (i < TimeSec.Count && TimeSec[i] <= endSec)
        {
            char cc = ConfidenceClass[i];
            if (cc is not ('A' or 'B')) { ++i; continue; }
            int source = SelectedSource[i];
            int first = i;
            while (i + 1 < TimeSec.Count && ConfidenceClass[i + 1] == cc && SelectedSource[i + 1] == source) ++i;
            double finish = DecisionEnd(i);
            if (finish >= startSec) result.Add(new DecisionInterval(TimeSec[first], finish, cc, source));
            ++i;
        }
        return result;
    }

    // B decisions are produced in approximately 2 ms cells.  A short run of C
    // cells between two B cells is still one practical listening/review area,
    // while an intervening accepted A repair must always split the regions.
    internal List<DecisionInterval> FlaggedIntervals(double startSec,double endSec,
        double maximumCleanGapSec=0.004)
    {
        var merged=new List<DecisionInterval>();
        DecisionInterval? pending=null;
        foreach(var interval in DecisionIntervals(startSec,endSec))
        {
            if(interval.Class=='A')
            {
                if(pending is { } beforeRepair) merged.Add(beforeRepair);
                pending=null;
                continue;
            }
            if(interval.Class!='B') continue;
            if(pending is { } current &&
               interval.Start-current.End<=maximumCleanGapSec)
                pending=new DecisionInterval(current.Start,interval.End,'B',
                    current.SelectedSource);
            else
            {
                if(pending is { } complete) merged.Add(complete);
                pending=interval;
            }
        }
        if(pending is { } last) merged.Add(last);
        return merged;
    }

    internal double FindInterestingTime(double fromSec, int direction)
    {
        if (TimeSec.Count == 0) return 0.0;
        int start = NearestIndex(fromSec);
        if (start < 0) start = 0;
        for (int i = start + Math.Sign(direction); i >= 0 && i < TimeSec.Count; i += Math.Sign(direction))
        {
            char cc = i < ConfidenceClass.Count ? ConfidenceClass[i] : 'C';
            char previous = i > 0 && i - 1 < ConfidenceClass.Count ? ConfidenceClass[i - 1] : 'C';
            if ((cc == 'A' || cc == 'B') && previous != cc) return TimeSec[i];
        }
        return Math.Clamp(fromSec, 0.0, DurationSec);
    }

    internal double FindClassTime(double fromSec,char wantedClass,int direction,bool wrap=true)
    {
        if(TimeSec.Count==0) return 0.0;
        int sign=direction<0?-1:1;
        int start=NearestIndex(fromSec);
        if(start<0) start=sign>0?-1:TimeSec.Count;
        bool IsStart(int index)
        {
            if(index<0 || index>=ConfidenceClass.Count || ConfidenceClass[index]!=wantedClass)
                return false;
            return index==0 || ConfidenceClass[index-1]!=wantedClass;
        }
        for(int i=start+sign;i>=0 && i<TimeSec.Count;i+=sign)
            if(IsStart(i)) return TimeSec[i];
        if(wrap)
        {
            if(sign>0)
                for(int i=0;i<=Math.Min(start,TimeSec.Count-1);++i)
                    if(IsStart(i)) return TimeSec[i];
            else
                for(int reverseIndex=TimeSec.Count-1;reverseIndex>=Math.Max(0,start);--reverseIndex)
                    if(IsStart(reverseIndex)) return TimeSec[reverseIndex];
        }
        return Math.Clamp(fromSec,0.0,DurationSec);
    }

    internal int FlaggedIntervalCount => FlaggedIntervals(0.0,DurationSec).Count;

    private readonly record struct Run(double Start, double End, int Source);

    private List<Run> BuildRuns(out double duration)
    {
        if (_cachedRuns is not null)
        {
            duration = _cachedRunDuration;
            return _cachedRuns;
        }

        var runs = new List<Run>();
        duration = 0.0;
        if (TimeSec.Count == 0 || SelectedSource.Count != TimeSec.Count) return runs;

        // Infer the last segment duration from the actual decision-map spacing.
        // Median is robust against the occasional irregular timestamp.
        var steps = new List<double>();
        for (int i = 1; i < TimeSec.Count; ++i)
        {
            double dt = TimeSec[i] - TimeSec[i - 1];
            if (dt > 0 && double.IsFinite(dt)) steps.Add(dt);
        }
        double step = 0.002; // fallback for current ~2 ms engine decisions
        if (steps.Count > 0)
        {
            steps.Sort();
            step = steps[steps.Count / 2];
        }
        duration = Math.Max(step, TimeSec[^1] + step);

        int start = 0;
        for (int i = 1; i <= SelectedSource.Count; ++i)
        {
            bool endRun = i == SelectedSource.Count || SelectedSource[i] != SelectedSource[start];
            if (!endRun) continue;

            double rs = Math.Max(0.0, TimeSec[start]);
            double re = i < TimeSec.Count ? Math.Max(rs, TimeSec[i]) : duration;
            runs.Add(new Run(rs, re, SelectedSource[start]));
            start = i;
        }
        _cachedRuns = runs;
        _cachedRunDuration = duration;
        return runs;
    }

    internal void PrepareForDisplay()
    {
        _ = BuildRuns(out _);
    }

    private static double SourceY(int source, int maxSource, double height)
    {
        // Reserve a separate lower band for ticks, labels and live viewport.
        const double top = 14.0;
        double bottom = Math.Max(top, height - 58.0);
        if (maxSource <= 1) return (top + bottom) * 0.5;
        return top + (source - 1.0) * (bottom - top) / (maxSource - 1.0);
    }

    internal static void Clear(Polyline line, Canvas canvas)
    {
        line.Points.Clear();
        for (int i = canvas.Children.Count - 1; i >= 0; --i)
        {
            bool persistent=canvas.Children[i] is FrameworkElement element &&
                string.Equals(element.Tag?.ToString(),"PersistentTimelineOverlay",StringComparison.Ordinal);
            if (!ReferenceEquals(canvas.Children[i], line) && !persistent) canvas.Children.RemoveAt(i);
        }
    }

    internal double DurationSec
    {
        get
        {
            if (TimeSec.Count == 0) return 0.0;
            double step = TimeSec.Count > 1 ? Math.Max(0.000001, TimeSec[^1] - TimeSec[^2]) : 0.002;
            return Math.Max(step, TimeSec[^1] + step);
        }
    }

    internal void Draw(Polyline line, Canvas canvas)
        => DrawViewport(line, canvas, 0.0, canvas.ActualWidth);

    internal void DrawViewport(Polyline line, Canvas canvas, double horizontalOffset, double viewportWidth)
    {
        Clear(line, canvas);
        if (TimeSec.Count == 0 || canvas.ActualWidth <= 1 || canvas.ActualHeight <= 1) return;

        var runs = BuildRuns(out double duration);
        if (runs.Count == 0 || duration <= 0) return;

        double w = canvas.ActualWidth;
        double h = canvas.ActualHeight;
        int maxSource = Math.Max(1, SelectedSource.Max());

        double X(double t) => Math.Clamp(w * t / duration, 0.0, w);
        double viewLeft = Math.Max(0.0, horizontalOffset - 8.0);
        double viewRight = Math.Min(w, horizontalOffset + Math.Max(1.0, viewportWidth) + 8.0);
        double viewStartSec = duration * viewLeft / w;
        double viewEndSec = duration * viewRight / w;
        var visibleRuns = runs.Where(r => r.End >= viewStartSec && r.Start <= viewEndSec).ToList();
        if (visibleRuns.Count == 0) return;

        // Exact source step-curve, but only for the visible viewport.
        // This keeps live rendering cost bounded on multi-minute recordings., compressed by runs rather than down-sampling.
        // No source decision can disappear because of a plotting stride.
        var first = visibleRuns[0];
        line.Points.Add(new Point(X(first.Start), SourceY(first.Source, maxSource, h)));
        for (int i = 0; i < visibleRuns.Count; ++i)
        {
            var run = visibleRuns[i];
            double y = SourceY(run.Source, maxSource, h);
            double xs = X(run.Start);
            double xe = X(run.End);

            if (i > 0)
            {
                double prevY = SourceY(visibleRuns[i - 1].Source, maxSource, h);
                line.Points.Add(new Point(xs, prevY));
                line.Points.Add(new Point(xs, y));
            }
            line.Points.Add(new Point(xe, y));
        }

        // A physically correct time scale means a nanosecond-wide donor interval on a
        // multi-minute recording is far below one screen pixel. Such an interval cannot
        // have literal visible width, so add a minimum-width event marker at its true
        // time position. The marker changes visibility only, never the underlying time.
        // This guarantees that EVERY donor excursion is visible on screen.
        var brush = line.Stroke ?? new SolidColorBrush(Windows.UI.Color.FromArgb(255, 0, 103, 192));
        const double minVisibleWidthPx = 2.0;

        // Thousands of individual XAML Line controls can exhaust WinUI after a long
        // full-track run. Merge events that occupy the same screen pixel and render
        // all A markers as one Path UI element instead.
        var donorColumns = new SortedDictionary<int, int>();
        foreach (var run in visibleRuns)
        {
            if (run.Source == 1) continue;
            double xs = X(run.Start);
            double xe = X(run.End);
            if (xe - xs >= minVisibleWidthPx) continue;
            int column = (int)Math.Round(0.5 * (xs + xe));
            if (!donorColumns.TryGetValue(column, out int oldSource) || run.Source > oldSource)
                donorColumns[column] = run.Source;
        }
        if (donorColumns.Count > 0)
        {
            var geometry = new PathGeometry();
            double yMaster = SourceY(1, maxSource, h);
            foreach (var pair in donorColumns)
            {
                var figure = new PathFigure { StartPoint = new Point(pair.Key, yMaster), IsClosed = false };
                figure.Segments.Add(new LineSegment { Point = new Point(pair.Key, SourceY(pair.Value, maxSource, h)) });
                geometry.Figures.Add(figure);
            }
            canvas.Children.Add(new Microsoft.UI.Xaml.Shapes.Path
            {
                Data = geometry,
                Stroke = brush,
                StrokeThickness = minVisibleWidthPx,
                IsHitTestVisible = false
            });
        }

        // B = probable defect, deliberately not repaired: short marker at Master.
        // C = difference only / normal variation: intentionally not drawn.
        if (ConfidenceClass.Count == TimeSec.Count)
        {
            double yMaster = SourceY(1, maxSource, h);
            var bColumns = new SortedSet<int>();
            for (int i = 0; i < TimeSec.Count; ++i)
            {
                if (ConfidenceClass[i] != 'B') continue;
                double t = TimeSec[i];
                if (t < viewStartSec || t > viewEndSec) continue;
                bColumns.Add((int)Math.Round(X(t)));
            }
            if (bColumns.Count > 0)
            {
                var geometry = new PathGeometry();
                foreach (int x in bColumns)
                {
                    var figure = new PathFigure { StartPoint = new Point(x, yMaster), IsClosed = false };
                    figure.Segments.Add(new LineSegment { Point = new Point(x, Math.Min(h - 50, yMaster + 8)) });
                    geometry.Figures.Add(figure);
                }
                canvas.Children.Add(new Microsoft.UI.Xaml.Shapes.Path
                {
                    Data = geometry,
                    Stroke = brush,
                    StrokeThickness = 1,
                    Opacity = 0.38,
                    IsHitTestVisible = false
                });
            }
        }

        // Time scale. Tick spacing adapts to the current horizontal scale.
        double pxPerSec = w / duration;
        double tickSec = pxPerSec >= 120 ? 1 : pxPerSec >= 45 ? 5 : pxPerSec >= 18 ? 10 : 30;
        double axisY = Math.Max(0, h - 40);
        double firstTick = Math.Max(0.0, Math.Floor(viewStartSec / tickSec) * tickSec);
        double lastTick = Math.Min(duration, viewEndSec + tickSec);
        for (double t = firstTick; t <= lastTick + 1e-9; t += tickSec)
        {
            double x = X(t);
            var tick = new Line { X1=x, X2=x, Y1=axisY-4, Y2=axisY, Stroke=brush, StrokeThickness=1, Opacity=0.45, IsHitTestVisible=false };
            canvas.Children.Add(tick);
            var label = new TextBlock
            {
                Text = $"{TimeSpan.FromSeconds(t):m\\:ss}", FontSize=12, Opacity=0.85, IsHitTestVisible=false
            };
            Canvas.SetLeft(label, Math.Clamp(x-16, 0.0, Math.Max(0.0, w-40.0)));
            Canvas.SetTop(label, axisY+2);
            canvas.Children.Add(label);
        }
    }
}
