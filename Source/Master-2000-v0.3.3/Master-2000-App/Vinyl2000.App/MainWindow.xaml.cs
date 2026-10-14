using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Windows.ApplicationModel.DataTransfer;
using Windows.Storage;
using Windows.Storage.Pickers;

namespace Vinyl2000.App;

public sealed partial class MainWindow : Window
{
    private readonly ObservableCollection<SourceItem> _sources=[];
    private Process? _engineProcess;
    private string? _lastOutput;
    private string? _lastMap;
    private DecisionMapData? _mapData;
    private readonly Stopwatch _analysisWatch = new();
    private DecisionMapData? _liveMapData;
    private long _lastLiveDrawTicks;
    private long _lastLiveEnqueueTicks;
    private bool _timelineDrawingDisabled;
    private bool _timelineUserScrolled;
    private double _waveformCenterSec;
    private double _outputSpeedFactor=1.0;
    private readonly DispatcherTimer _waveformPanTimer=new() { Interval=TimeSpan.FromMilliseconds(16) };
    private readonly DispatcherTimer _analysisHeartbeatTimer=new() { Interval=TimeSpan.FromSeconds(1) };
    private bool _waveformPanActive;
    private Canvas? _waveformPointerCanvas;
    private double _waveformPanPointerX;
    private double _waveformPanStartX;
    private double _waveformPanStartCenterSec;
    private long _waveformPanPressedTicks;
    private long _waveformPanLastTicks;
    private readonly Dictionary<string,string> _sourceHashes = new(StringComparer.OrdinalIgnoreCase);
    private bool _repairOverlayEnabled=true;
    private bool _repairOverlayInitializing=true;
    private bool _levelMatchEnabled=true;
    private string _analysisHeartbeatPhase="";
    private readonly Dictionary<int,string> _sourceRoles=[];
    private readonly HashSet<DecisionMapData.RepairInterval> _revertedRepairs=[];
    private string _outputQaSummary="";
    private const string ContactEmail = "master2000project@proton.me";

    public MainWindow()
    {
        InitializeComponent();
        InitializeListening();
        try
        {
            string setting=RepairOverlaySettingPath();
            _repairOverlayEnabled=!File.Exists(setting) || File.ReadAllText(setting).Trim()!="0";
            RepairOverlayToggle.IsOn=_repairOverlayEnabled;
        }
        catch { _repairOverlayEnabled=true; }
        finally { _repairOverlayInitializing=false; }
        _waveformPanTimer.Tick+=WaveformPanTimer_Tick;
        _analysisHeartbeatTimer.Tick+=AnalysisHeartbeatTimer_Tick;
        SourcesList.ItemsSource=_sources;
        UpdateSourceState();
    }

    private string AnalysisElapsedText()
    {
        var elapsed=_analysisWatch.Elapsed;
        return $"{(int)elapsed.TotalHours:00}:{elapsed.Minutes:00}:{elapsed.Seconds:00}";
    }

    private sealed record DonorRepairSummary(int Source,int Repairs,double DurationMs,double FirstUseSec);
    private sealed record RepairRunSummary(int Repairs,double DurationMs,IReadOnlyList<DonorRepairSummary> Donors);

    private static string RepairLogPath(string decisionMapPath)
    {
        const string suffix=".decisions.csv";
        return decisionMapPath.EndsWith(suffix,StringComparison.OrdinalIgnoreCase)
            ? decisionMapPath[..^suffix.Length]+".repairs.csv"
            : decisionMapPath+".repairs.csv";
    }

    private static RepairRunSummary? ReadRepairRunSummary(string? decisionMapPath)
    {
        if(string.IsNullOrWhiteSpace(decisionMapPath)) return null;
        string path=RepairLogPath(decisionMapPath);
        if(!File.Exists(path)) return null;

        var byDonor=new SortedDictionary<int,(int Repairs,double DurationMs,double FirstUseSec)>();
        int repairCount=0;
        double totalDurationMs=0.0;
        foreach(string line in File.ReadLines(path).Skip(1))
        {
            string[] fields=line.Split(',');
            if(fields.Length<4 ||
               !double.TryParse(fields[0],NumberStyles.Float,CultureInfo.InvariantCulture,out double startSec) ||
               !double.TryParse(fields[2],NumberStyles.Float,CultureInfo.InvariantCulture,out double durationMs) ||
               !int.TryParse(fields[3],NumberStyles.Integer,CultureInfo.InvariantCulture,out int donor) ||
               !double.IsFinite(startSec) || startSec<0.0 ||
               !double.IsFinite(durationMs) || durationMs<0.0 || donor<=0) continue;

            ++repairCount;
            totalDurationMs+=durationMs;
            byDonor.TryGetValue(donor,out var totals);
            double firstUse=totals.Repairs==0 ? startSec : Math.Min(totals.FirstUseSec,startSec);
            byDonor[donor]=(totals.Repairs+1,totals.DurationMs+durationMs,firstUse);
        }

        var donors=byDonor.Select(pair=>new DonorRepairSummary(
            pair.Key,pair.Value.Repairs,pair.Value.DurationMs,pair.Value.FirstUseSec)).ToList();
        return new RepairRunSummary(repairCount,totalDurationMs,donors);
    }

    private static string DisplayDuration(double milliseconds)
    {
        if(milliseconds>=1000.0)
            return (milliseconds/1000.0).ToString("0.000",CultureInfo.CurrentCulture)+" s";
        if(milliseconds<=0.0005) return "0 ms";
        return milliseconds.ToString("0.0",CultureInfo.CurrentCulture)+" ms";
    }

    private void PresentRunSummary(RepairRunSummary? summary)
    {
        int availableDonors=Math.Max(0,_sources.Count-1);
        if(summary is null)
        {
            RunSummaryHeadline.Text="Detailed donor contribution is unavailable for this run.";
            RunSummaryDonors.Text="The result and decision map can still be inspected normally.";
            RunSummaryFirstUse.Text="";
        }
        else
        {
            int usedDonors=summary.Donors.Count;
            RunSummaryHeadline.Text=$"{summary.Repairs:N0} verified repairs • {DisplayDuration(summary.DurationMs)} donor audio inserted • {usedDonors} of {availableDonors} non-Master sources used";
            RunSummaryDonors.Text=usedDonors==0
                ? "No donor segment passed every safety check."
                : "Contribution by donor — "+string.Join("   •   ",summary.Donors.Select(donor=>
                {
                    double percent=summary.DurationMs>0.0
                        ? 100.0*donor.DurationMs/summary.DurationMs : 0.0;
                    return $"Source {donor.Source}: {donor.Repairs:N0} repairs / {DisplayDuration(donor.DurationMs)} ({percent:0.0}%)";
                }));
            RunSummaryFirstUse.Text=usedDonors==0 ? "" :
                "First accepted donor insertion — "+string.Join("   •   ",summary.Donors.Select(donor=>
                    $"Source {donor.Source}: {donor.FirstUseSec:0.000} s"));
        }
        RunSummaryNote.Text="All other samples remain the selected Master. Exact intervals and donor choices are saved in the repair log and decision map.";
        RunSummaryPanel.Visibility=Visibility.Visible;
    }

    private void HideRunSummary()
    {
        RunSummaryPanel.Visibility=Visibility.Collapsed;
        RunSummaryHeadline.Text="";
        RunSummaryDonors.Text="";
        RunSummaryFirstUse.Text="";
        RunSummaryNote.Text="";
    }

    private void StartAnalysisHeartbeat(string phase)
    {
        _analysisHeartbeatPhase=phase;
        AnalysisHeartbeatTimer_Tick(null,null!);
        _analysisHeartbeatTimer.Start();
    }

    private void SetAnalysisHeartbeatPhase(string phase)
    {
        _analysisHeartbeatPhase=phase;
        if(_analysisHeartbeatTimer.IsEnabled)
            AnalysisHeartbeatTimer_Tick(null,null!);
    }

    private void StopAnalysisHeartbeat()
    {
        _analysisHeartbeatTimer.Stop();
        _analysisHeartbeatPhase="";
        if(HeartbeatText is not null) HeartbeatText.Text="";
    }

    private void AnalysisHeartbeatTimer_Tick(object? sender,object e)
    {
        if(HeartbeatText is null || string.IsNullOrWhiteSpace(_analysisHeartbeatPhase)) return;
        HeartbeatText.Text=$"{_analysisHeartbeatPhase} • elapsed {AnalysisElapsedText()}";
    }

    private static string RepairOverlaySettingPath()
    {
        string dir=System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"Master-2000");
        return System.IO.Path.Combine(dir,"repair-overlay.enabled");
    }

    private IntPtr Hwnd => WinRT.Interop.WindowNative.GetWindowHandle(this);

    private async void Forensic_Click(object sender,RoutedEventArgs e)
    {
        if(_engineProcess is not null || _sources.Count==0) return;
        var paths=_sources.Select(s=>s.OriginalPath).ToArray();
        ForensicButton.IsEnabled=false;
        try {
            string report=await Task.Run(()=>string.Join("\n\n",paths.Select(path=> {
                try {return ForensicCheck.Inspect(path);} catch(Exception ex) {return Path.GetFileName(path)+": "+ex.Message;}
            })));
            var dialog=new ContentDialog {
                XamlRoot=Content.XamlRoot,Title="Forensic check — experimental",CloseButtonText="Close",
                Content=new ScrollViewer {MaxHeight=480,Content=new TextBox {
                    Text=report,IsReadOnly=true,TextWrapping=TextWrapping.Wrap,AcceptsReturn=true,MinWidth=450
                }}
            };
            await dialog.ShowAsync();
        } finally {ForensicButton.IsEnabled=_engineProcess is null;}
    }

    private async void AddSources_Click(object sender,RoutedEventArgs e)
    {
        var picker=new FileOpenPicker();
        WinRT.Interop.InitializeWithWindow.Initialize(picker,Hwnd);
        foreach(string ext in LosslessAudio.Extensions) picker.FileTypeFilter.Add(ext);
        var files=await picker.PickMultipleFilesAsync();
        foreach(var file in files) await AddSourceAsync(file.Path);
    }

    private static string Sha256FileNow(string path)
    {
        using var sha=SHA256.Create();
        using var fs=File.OpenRead(path);
        return Convert.ToHexString(sha.ComputeHash(fs)).ToLowerInvariant();
    }

    private async Task AddSourceAsync(string path)
    {
        if(_engineProcess is not null) return;
        if(!LosslessAudio.IsSupportedPath(path)) return;
        if(_sources.Any(s=>string.Equals(s.OriginalPath,path,StringComparison.OrdinalIgnoreCase))) return;
        if(_sources.Count>=6)
        {
            ResultInfo.Severity=InfoBarSeverity.Warning;
            ResultInfo.Title="Six-source limit";
            ResultInfo.Message="This validation build accepts three to six independent physical copies.";
            ResultInfo.IsOpen=true;
            return;
        }

        string hash;
        try { hash=await Task.Run(()=>Sha256FileNow(path)); }
        catch(Exception ex)
        {
            ResultInfo.Severity=InfoBarSeverity.Error;
            ResultInfo.Title="Could not read WAV";
            ResultInfo.Message=ex.Message;
            ResultInfo.IsOpen=true;
            return;
        }

        if(_sourceHashes.TryGetValue(hash,out string? existing))
        {
            ResultInfo.Severity=InfoBarSeverity.Warning;
            ResultInfo.Title="Source already added";
            ResultInfo.Message=$"{System.IO.Path.GetFileName(path)} is byte-identical to {System.IO.Path.GetFileName(existing)} (SHA-256 match).";
            ResultInfo.IsOpen=true;
            return;
        }

        try
        {
            var prepared=await LosslessAudio.PrepareAsync(path,hash);
            _sources.Add(new SourceItem{
                Number=_sources.Count+1,
                OriginalPath=path,
                Path=prepared.EnginePath,
                Details=prepared.Description,
                Format=prepared.Format});
        }
        catch(Exception ex)
        {
            ResultInfo.Severity=InfoBarSeverity.Error;
            ResultInfo.Title="Could not decode lossless audio";
            ResultInfo.Message=System.IO.Path.GetFileName(path)+": "+ex.Message;
            ResultInfo.IsOpen=true;
            return;
        }
        _sourceHashes[hash]=path;
        Renumber(); UpdateSourceState();
    }

    private void RebuildSourceHashes()
    {
        _sourceHashes.Clear();
        foreach(var src in _sources)
        {
            try { _sourceHashes[Sha256FileNow(src.OriginalPath)]=src.OriginalPath; } catch { }
        }
    }

    private void RemoveSource_Click(object sender,RoutedEventArgs e)
    {
        if(_engineProcess is not null) return;
        if(SourcesList.SelectedItem is SourceItem item){_sources.Remove(item);RebuildSourceHashes();Renumber();UpdateSourceState();}
    }
    private void ClearSources_Click(object sender,RoutedEventArgs e)
    {
        if(_engineProcess is not null) return;
        if(_engineProcess is not null) return;

        StopWaveformPan();
        _sources.Clear();
        _sourceHashes.Clear();
        _lastOutput=null;
        _lastMap=null;
        _mapData=null;
        _liveMapData=null;
        _lastLiveDrawTicks=0;
        _lastLiveEnqueueTicks=0;
        _timelineDrawingDisabled=false;
        _timelineUserScrolled=false;
        _waveformCenterSec=0.0;
        _outputSpeedFactor=1.0;
        _analysisWatch.Reset();
        StopAnalysisHeartbeat();
        _sourceRoles.Clear();
        _revertedRepairs.Clear();
        _outputQaSummary="";

        Renumber();
        UpdateSourceState();
        DecisionMapData.Clear(DriftLine,DriftCanvas);
        WaveformViewportLine.Visibility=Visibility.Collapsed;
        MasterWaveformCanvas.Children.Clear();
        DonorWaveformCanvas.Children.Clear();
        ResultWaveformCanvas.Children.Clear();
        UpdateTimelineWidth(null);
        TimelineScroll.ChangeView(0.0,null,null,true);
        NoAnalysisHint.Visibility=Visibility.Visible;

        StatusText.Text="Ready";
        DetailText.Text="No processing has been run.";
        PositionText.Text="";
        HeartbeatText.Text="";
        ProfileText.Text="";
        WaveformPositionText.Text="Wheel zooms at pointer • hold left button to shuttle • short click selects time";
        WaveformOutputText.Text="Output at cursor: no result loaded";
        WaveformOutputText.Foreground=new Microsoft.UI.Xaml.Media.SolidColorBrush(
            Windows.UI.Color.FromArgb(255,65,90,120));
        AnalysisProgress.IsIndeterminate=false;
        AnalysisProgress.Value=0;
        ProgressText.Text="";
        ResultInfo.IsOpen=false;
        HideRunSummary();
        OpenResultButton.IsEnabled=false;
        OpenMapButton.IsEnabled=false;
        PreviousRepairButton.IsEnabled=false;
        NextRepairButton.IsEnabled=false;
        ReviewFlaggedButton.IsEnabled=false;
        RevertRepairButton.IsEnabled=false;
        RestoreRepairButton.IsEnabled=false;
        SaveCorrectedButton.IsEnabled=false;
        PlayMasterResultButton.IsEnabled=false;
        PlayResultCursorButton.IsEnabled=false;
        CorrectionStatusText.Text="";
    }

    private void Renumber()
    {
        for(int i=0;i<_sources.Count;i++) _sources[i].Number=i+1;
        SourcesList.ItemsSource=null; SourcesList.ItemsSource=_sources;
    }

    private void UpdateSourceState()
    {
        ReleaseStatusText.Visibility=Visibility.Collapsed;
        DropHint.Visibility=_sources.Count==0?Visibility.Visible:Visibility.Collapsed;
        bool valid=_sources.All(s=>s.Format is not null);
        var first=_sources.FirstOrDefault()?.Format;
        bool compatible=valid && first is not null && _sources.All(s=>s.Format is not null && s.Format.SampleRate==first.SampleRate && s.Format.Bits==first.Bits && s.Format.Channels==first.Channels);
        if(MasterMode.SelectedIndex>_sources.Count) MasterMode.SelectedIndex=0;
        AnalyzeButton.IsEnabled=_engineProcess is null &&
            _sources.Count is >=3 and <=6 && compatible;
        SourceCountText.Text=_sources.Count switch
        {
            0=>"Add three to six different copies of the same pressing",
            1=>"1 unique source — add at least two more",
            2=>"2 unique sources — add one more",
            6=>compatible?"6 unique sources ready — maximum (reserve enabled)":
                "6 sources — FORMAT MISMATCH",
            _=>compatible?$"{_sources.Count} unique sources ready":
                $"{_sources.Count} sources — FORMAT MISMATCH"
        };
        InventoryText.Text=_sources.Count==0 ? "Input format: —" : compatible && first is not null ? $"Analysis format: {first.Short} — {_sources.Count} files compatible" : "⚠ Inputs must decode to identical sample rate, bit depth and channel count.";
        if(_sources.Count==0 || first is null) QualityText.Text="";
        else if(first.SampleRate>=96000 && first.Bits>=24) QualityText.Text="✓ Archival capture target met: 96 kHz / 24-bit or better.";
        else QualityText.Text="⚠ Accepted. For new archival captures, Master-2000 recommends 96 kHz / 24-bit PCM WAV.";
        UpdateDonorScrollUi();
    }

    private void DropArea_DragOver(object sender,DragEventArgs e)
    {
        if(e.DataView.Contains(StandardDataFormats.StorageItems)){e.AcceptedOperation=DataPackageOperation.Copy;e.DragUIOverride.Caption="Add lossless audio source";e.DragUIOverride.IsCaptionVisible=true;}
    }
    private async void DropArea_Drop(object sender,DragEventArgs e)
    {
        if(!e.DataView.Contains(StandardDataFormats.StorageItems)) return;
        var items=await e.DataView.GetStorageItemsAsync();
        foreach(var item in items) if(item is StorageFile f && LosslessAudio.IsSupportedPath(f.Path)) await AddSourceAsync(f.Path);
    }

    private async void Analyze_Click(object sender,RoutedEventArgs e)
    {
        if(_sources.Count is <3 or >6) return;

        var sync=await PrepareSyncPreviewAsync();
        if(sync is null) return;

        var picker=new FileSavePicker();
        WinRT.Interop.InitializeWithWindow.Initialize(picker,Hwnd);
        picker.SuggestedFileName="Master-2000-restored";
        picker.FileTypeChoices.Add("PCM WAV",new List<string>{".wav"});
        var output=await picker.PickSaveFileAsync();
        if(output is null) return;

        _lastOutput=output.Path;
        _lastMap=System.IO.Path.ChangeExtension(output.Path,".decisions.csv");

        string engine=System.IO.Path.Combine(AppContext.BaseDirectory,"Master2000Engine.exe");
        if(!File.Exists(engine))
        {
            ShowFailure("Master2000Engine.exe was not found beside the app.");
            return;
        }

        var psi=new ProcessStartInfo(engine)
        {
            UseShellExecute=false,
            CreateNoWindow=true,
            RedirectStandardError=true,
            RedirectStandardOutput=true
        };

        psi.ArgumentList.Add(_sources.Count.ToString());
        psi.ArgumentList.Add(_lastOutput);
        psi.ArgumentList.Add("--master");
        int masterChoice=sync.Master;
        psi.ArgumentList.Add(masterChoice<=0 ? "auto" : masterChoice.ToString());
        if(masterChoice>0)
        {
            psi.ArgumentList.Add("--master-origin");
            psi.ArgumentList.Add(sync.WasAutomatic ? "auto-frozen" : "manual");
        }
        double recordedRpm=KeepFileSpeed.IsChecked==true ? 0.0 : SelectedRpmValue(RecordedRpm);
        double discRpm=KeepFileSpeed.IsChecked==true ? 0.0 : SelectedRpmValue(DiscRpm);
        _outputSpeedFactor=recordedRpm>0.0 && discRpm>0.0 ? discRpm/recordedRpm : 1.0;
        bool stereoMode=ChannelModeToggle.IsOn;
        psi.ArgumentList.Add("--channel-mode");
        psi.ArgumentList.Add(stereoMode ? "stereo" : "mono");
        psi.ArgumentList.Add("--recorded-rpm");
        psi.ArgumentList.Add(recordedRpm.ToString("R",CultureInfo.InvariantCulture));
        psi.ArgumentList.Add("--disc-rpm");
        psi.ArgumentList.Add(discRpm.ToString("R",CultureInfo.InvariantCulture));
        psi.ArgumentList.Add("--map");
        psi.ArgumentList.Add(_lastMap);
        psi.ArgumentList.Add("--sync-offsets");
        psi.ArgumentList.Add(string.Join(",",sync.Offsets.Select(x=>double.IsNaN(x)?"auto":x.ToString("R",CultureInfo.InvariantCulture))));
        foreach(var s in _sources) psi.ArgumentList.Add(s.Path);

        SetBusy(true);
        ResultInfo.IsOpen=false;
        HideRunSummary();
        NoAnalysisHint.Visibility=Visibility.Visible;
        _mapData=null;
        _liveMapData=new DecisionMapData();
        _lastLiveDrawTicks=0;
        _lastLiveEnqueueTicks=0;
        _timelineDrawingDisabled=false;
        _timelineUserScrolled=false;
        _sourceRoles.Clear();
        _revertedRepairs.Clear();
        _outputQaSummary="";
        DonorWaveformScroll.ChangeView(null,0.0,null,true);
        PreviousRepairButton.IsEnabled=false;
        NextRepairButton.IsEnabled=false;
        UpdateCorrectionControls();
        UpdateTimelineWidth(_liveMapData);
        DecisionMapData.Clear(DriftLine,DriftCanvas);

        StatusText.Text="Preparing sources...";
        DetailText.Text=$"{_sources.Count} independent sources";
        PositionText.Text="Starting engine — waiting for first analysis phase...";
        AnalysisProgress.IsIndeterminate=true;
        AnalysisProgress.Minimum=0;
        AnalysisProgress.Maximum=100;
        AnalysisProgress.Value=0;
        ProgressText.Text="0%";
        _analysisWatch.Restart();
        StartAnalysisHeartbeat("Starting engine");
        ProfileText.Text="Profiling: waiting for phase timings...";

        var errorLines = new List<string>();
        var profileTimes = new System.Collections.Concurrent.ConcurrentDictionary<string,double>();
        string coarseSummary = "";
        string coarseDiagnosticPath = "";
        string fastPathSummary = "";
        string timeMapTotalSummary = "";
        string reconProfileSummary = "";
        string reconOuterSummary = "";
        string blockSyncSummary = "";
        int completedRepairs = -1;
        long completedChangedFrames = -1;
        string engineError = "";
        string outputQaLine = "";

        try
        {
            _engineProcess=new Process{StartInfo=psi,EnableRaisingEvents=true};

            _engineProcess.ErrorDataReceived += (_,args) =>
            {
                if(string.IsNullOrWhiteSpace(args.Data)) return;
                string line=args.Data.Trim();

                if(line.StartsWith("WORK_PHASE ",StringComparison.Ordinal)) {
                    string code=line.Substring("WORK_PHASE ".Length).Trim();
                    DispatcherQueue.TryEnqueue(()=>ApplyWorkPhase(code));
                }
                else if(line.StartsWith("WORK_PROGRESS ",StringComparison.Ordinal)) {
                    var fields=line.Split(' ',StringSplitOptions.RemoveEmptyEntries);
                    if(fields.Length==4 && long.TryParse(fields[2],out long done) &&
                       long.TryParse(fields[3],out long total) && total>0)
                        DispatcherQueue.TryEnqueue(()=>ApplyWorkProgress(fields[1],done,total));
                }
                else if(line.StartsWith("PREFLIGHT_PROGRESS ",StringComparison.Ordinal))
                {
                    var q=line.Split(' ',StringSplitOptions.RemoveEmptyEntries);
                    if(q.Length>=5 && int.TryParse(q[1],out int done) &&
                       int.TryParse(q[2],out int total) &&
                       int.TryParse(q[3],out int candidate) &&
                       int.TryParse(q[4],out int donor))
                    {
                        DispatcherQueue.TryEnqueue(() =>
                        {
                            AnalysisProgress.IsIndeterminate=false;
                            double phasePercent=total>0
                                ? 100.0*Math.Clamp((double)done/total,0.0,1.0) : 0.0;
                            AnalysisProgress.Value=phasePercent;
                            ProgressText.Text=$"Preflight • phase {phasePercent:0.0}%";
                            StatusText.Text="Preflighting all source pairs...";
                            PositionText.Text=$"Pair {done}/{total} • Source {candidate} ↔ Source {donor}";
                            SetAnalysisHeartbeatPhase($"Preflight {done}/{total}");
                        });
                    }
                }
                else if(line.StartsWith("SOURCE_ROLE ",StringComparison.Ordinal))
                {
                    var q=line.Split(' ',StringSplitOptions.RemoveEmptyEntries);
                    if(q.Length>=3 && int.TryParse(q[1],out int source))
                    {
                        string role=q[2];
                        DispatcherQueue.TryEnqueue(() => _sourceRoles[source]=role);
                    }
                }
                else if(line.StartsWith("PHASE_MAP ",StringComparison.Ordinal))
                {
                    var q=line.Split(' ',StringSplitOptions.RemoveEmptyEntries);
                    if(q.Length>=4 && int.TryParse(q[1],out int si) &&
                       int.TryParse(q[2],out int sc) && int.TryParse(q[3],out int pp))
                    {
                        int original=si;
                        if(q.Length>=5) int.TryParse(q[4],out original);
                        string role=q.Length>=6?q[5]:"DONOR";
                        DispatcherQueue.TryEnqueue(() =>
                        {
                            AnalysisProgress.IsIndeterminate=false;
                            AnalysisProgress.Value=Math.Clamp(pp,0,100);
                            ProgressText.Text=$"Time map {si}/{sc} • phase {pp}%";
                            StatusText.Text="Building verified time maps...";
                            PositionText.Text=$"Tidskarta {original}/{_sources.Count} — {pp}% • {role}";
                            SetAnalysisHeartbeatPhase($"Tidskarta {original}/{_sources.Count} — {pp}%");
                        });
                    }
                }
                else if(line.StartsWith("PHASE ",StringComparison.Ordinal))
                {
                    var q=line.Split(' ',StringSplitOptions.RemoveEmptyEntries);
                    string phase=q.Length>1?q[1]:"";
                    DispatcherQueue.TryEnqueue(() =>
                    {
                        if(phase=="GLOBAL_ALIGNMENT")
                        {
                            AnalysisProgress.IsIndeterminate=true;
                            ProgressText.Text="Start sync • working";
                            StatusText.Text="Finding global start alignment...";
                            string n=(q.Length>=4)?$"{q[2]}/{q[3]}":"";
                            string source=q.Length>=5?$" • Source {q[4]}":"";
                            string role=q.Length>=6?$" ({q[5]})":"";
                            PositionText.Text=$"Aligning independent sources {n}{source}{role}";
                            SetAnalysisHeartbeatPhase($"Global alignment {n}");
                        }
                        else if(phase=="MASTER_PREFLIGHT")
                        {
                            AnalysisProgress.IsIndeterminate=true;
                            ProgressText.Text="Master preflight • working";
                            StatusText.Text="Checking which Master has two syncable donors...";
                            PositionText.Text="Whole-record sync preflight";
                            SetAnalysisHeartbeatPhase("Whole-record preflight");
                        }
                        else if(phase=="TIME_MAP") {
                            AnalysisProgress.IsIndeterminate=true;
                            AnalysisProgress.Value=0;
                            ProgressText.Text="Time maps • working";
                            StatusText.Text="Building verified time maps...";
                            SetAnalysisHeartbeatPhase("Time maps");
                        }
                        else if(phase=="RESTORATION")
                        {
                            AnalysisProgress.IsIndeterminate=false;
                            AnalysisProgress.Value=0;
                            ProgressText.Text="Analysis • phase 0.0%";
                            StatusText.Text="Micro-aligning and reconstructing...";
                            PositionText.Text="Verified maps ready";
                            SetAnalysisHeartbeatPhase("Micro-alignment and reconstruction");
                        }
                        else if(phase=="WRITING_OUTPUT")
                        {
                            AnalysisProgress.IsIndeterminate=true;
                            ProgressText.Text="Writing output • final phase";
                            StatusText.Text="Writing result and decision map...";
                            PositionText.Text="Audio processing complete";
                            SetAnalysisHeartbeatPhase("Writing WAV and diagnostics");
                        }
                    });
                }
                else if(line.StartsWith("BLOCKSYNC_PROFILE ",StringComparison.Ordinal))
                {
                    blockSyncSummary = line;
                }
                else if(line.StartsWith("MASTER_PRESERVE ",StringComparison.Ordinal))
                {
                    var q=line.Split(' ',StringSplitOptions.RemoveEmptyEntries);
                    int ri=Array.IndexOf(q,"repairs");
                    int ci=Array.IndexOf(q,"changed_frames");
                    if(ri>=0 && ri+1<q.Length) int.TryParse(q[ri+1],out completedRepairs);
                    if(ci>=0 && ci+1<q.Length) long.TryParse(q[ci+1],out completedChangedFrames);
                }
                else if(line.StartsWith("OUTPUT_QA ",StringComparison.Ordinal))
                {
                    outputQaLine=line;
                }
                else if(line.StartsWith("RECON_OUTER ",StringComparison.Ordinal))
                {
                    reconOuterSummary = line;
                }
                else if(line.StartsWith("RECON_PROFILE ",StringComparison.Ordinal))
                {
                    reconProfileSummary = line;
                }
                else if(line.StartsWith("TIMEMAP_TOTAL ",StringComparison.Ordinal))
                {
                    timeMapTotalSummary = line;
                }
                else if(line.StartsWith("FASTPATH ",StringComparison.Ordinal))
                {
                    fastPathSummary = line;
                }
                else if(line.StartsWith("COARSE_DIAGNOSTIC ",StringComparison.Ordinal))
                {
                    coarseDiagnosticPath = line.Substring("COARSE_DIAGNOSTIC ".Length).Trim();
                }
                else if(line.StartsWith("COARSE ",StringComparison.Ordinal))
                {
                    coarseSummary = line;
                }
                else if(line.StartsWith("PROFILE ",StringComparison.Ordinal))
                {
                    var q=line.Split(' ',StringSplitOptions.RemoveEmptyEntries);
                    if(q.Length>=3 &&
                       double.TryParse(q[2],System.Globalization.NumberStyles.Float,
                           System.Globalization.CultureInfo.InvariantCulture,out double secs))
                    {
                        profileTimes[q[1]]=secs;
                        DispatcherQueue.TryEnqueue(() =>
                        {
                            string Pretty(string key)
                            {
                                if(!profileTimes.TryGetValue(key,out double v)) return "--:--:--";
                                var ts=TimeSpan.FromSeconds(Math.Max(0,v));
                                return $"{(int)ts.TotalHours:00}:{ts.Minutes:00}:{ts.Seconds:00}";
                            }
                            ProfileText.Text=
                                $"Profiling — preflight {Pretty("master_preflight")} • align {Pretty("global_alignment")} • maps {Pretty("verified_time_maps")} • " +
                                $"reconstruct {Pretty("reconstruction")} • write {Pretty("write_output")}";
                        });
                    }
                }
                else if(line.StartsWith("MASTER_SELECTED ",StringComparison.Ordinal))
                {
                    var q=line.Split(' ',StringSplitOptions.RemoveEmptyEntries);
                    if(q.Length>=2 && int.TryParse(q[1],out int mi) && mi>=1 && mi<=_sources.Count)
                    {
                        string mn=_sources[mi-1].Name;
                        DispatcherQueue.TryEnqueue(()=>DetailText.Text=$"Master: Source {mi} — {mn}");
                    }
                }
                else if(line.StartsWith("PROGRESS ",StringComparison.Ordinal))
                {
                    string[] p=line.Split(' ',StringSplitOptions.RemoveEmptyEntries);
                    if(p.Length>=4 &&
                       double.TryParse(p[1],System.Globalization.NumberStyles.Float,System.Globalization.CultureInfo.InvariantCulture,out double percent) &&
                       double.TryParse(p[2],System.Globalization.NumberStyles.Float,System.Globalization.CultureInfo.InvariantCulture,out double doneSec) &&
                       double.TryParse(p[3],System.Globalization.NumberStyles.Float,System.Globalization.CultureInfo.InvariantCulture,out double totalSec))
                    {
                        DispatcherQueue.TryEnqueue(() =>
                        {
                            AnalysisProgress.IsIndeterminate=false;
                            double uiPercent=Math.Clamp(percent,0,100);
                            AnalysisProgress.Value=uiPercent;
                            ProgressText.Text=$"Analysis • phase {percent:0.0}%";
                            StatusText.Text="Micro-aligning and reconstructing...";

                            PositionText.Text=$"Audio analysed: {doneSec:0.0} / {totalSec:0.0} s";
                        });
                    }
                }
                else if(line.StartsWith("LIVE ",StringComparison.Ordinal))
                {
                    string[] p=line.Split(' ',StringSplitOptions.RemoveEmptyEntries);
                    if(p.Length>=3 &&
                       double.TryParse(p[1],System.Globalization.NumberStyles.Float,
                           System.Globalization.CultureInfo.InvariantCulture,out double liveSec) &&
                       int.TryParse(p[2],out int liveSource))
                    {
                        // The engine emits one decision roughly every 2 ms. Enqueuing
                        // every line can leave more than 100,000 UI callbacks pending on
                        // a full record side. Live display is only a preview; sample it
                        // at 5 Hz and load the exact decision CSV after completion.
                        long enqueueNow=Stopwatch.GetTimestamp();
                        long previous=System.Threading.Interlocked.Read(ref _lastLiveEnqueueTicks);
                        double sinceEnqueue=(enqueueNow-previous)/(double)Stopwatch.Frequency;
                        if(previous!=0 && sinceEnqueue<0.200) return;
                        System.Threading.Interlocked.Exchange(ref _lastLiveEnqueueTicks,enqueueNow);
                        DispatcherQueue.TryEnqueue(() =>
                        {
                            if(_liveMapData is null) return;
                            _liveMapData.AppendLive(liveSec,liveSource);
                            long now=Stopwatch.GetTimestamp();
                            double elapsed=(now-_lastLiveDrawTicks)/(double)Stopwatch.Frequency;
                            if(_lastLiveDrawTicks==0 || elapsed>=0.200)
                            {
                                UpdateTimelineWidth(_liveMapData);
                                DrawCurrentTimeline(_liveMapData);
                                NoAnalysisHint.Visibility=Visibility.Collapsed;
                                if(!_timelineUserScrolled) TimelineScroll.ChangeView(TimelineScroll.ScrollableWidth,null,null,true);
                                _lastLiveDrawTicks=now;
                            }
                        });
                    }
                }
                else if(line.StartsWith("ERROR:",StringComparison.Ordinal))
                {
                    engineError=line.Substring("ERROR:".Length).Trim();
                }
                else
                {
                    lock(errorLines) errorLines.Add(line);
                }
            };

            _engineProcess.Start();
            _engineProcess.BeginErrorReadLine();

            string stdOutTask=await _engineProcess.StandardOutput.ReadToEndAsync();
            await _engineProcess.WaitForExitAsync();
            int rc=_engineProcess.ExitCode;

            _analysisWatch.Stop();
            StopAnalysisHeartbeat();
            _engineProcess.Dispose();
            _engineProcess=null;
            SetBusy(false);
            AnalysisProgress.IsIndeterminate=false;

            if(rc==0 && File.Exists(_lastOutput))
            {
                _outputQaSummary=FormatOutputQa(outputQaLine);
                if(completedRepairs<0 && !string.IsNullOrWhiteSpace(_lastMap))
                {
                    string repairLog=RepairLogPath(_lastMap);
                    if(File.Exists(repairLog))
                        completedRepairs=Math.Max(0,File.ReadLines(repairLog).Count()-1);
                }
                var repairSummary=ReadRepairRunSummary(_lastMap);
                bool noSafeRepairs=completedRepairs==0 && completedChangedFrames<=0;
                AnalysisProgress.Value=100;
                ProgressText.Text="Completed";
                StatusText.Text=noSafeRepairs ? "Inga säkra reparationer – Master exporterad oförändrad." : "Restoration complete";
                DetailText.Text=System.IO.Path.GetFileName(_lastOutput);
                                var elapsed=_analysisWatch.Elapsed;
                PositionText.Text=$"Finished in {(int)elapsed.TotalHours:00}:{elapsed.Minutes:00}:{elapsed.Seconds:00} (hh:mm:ss)";
                string PF(string key)
                {
                    if(!profileTimes.TryGetValue(key,out double v)) return "--:--:--";
                    var ts=TimeSpan.FromSeconds(Math.Max(0,v));
                    return $"{(int)ts.TotalHours:00}:{ts.Minutes:00}:{ts.Seconds:00}";
                }
                ProfileText.Text=
                    $"Phase times — Master preflight {PF("master_preflight")} • alignment {PF("global_alignment")} • time maps {PF("verified_time_maps")} • " +
                    $"reconstruction {PF("reconstruction")} • output {PF("write_output")} • engine total {PF("total")}";
                if(!string.IsNullOrWhiteSpace(coarseSummary))
                {
                    var cq=coarseSummary.Split(' ',StringSplitOptions.RemoveEmptyEntries);
                    string V(string key)
                    {
                        int i=Array.IndexOf(cq,key);
                        return (i>=0 && i+1<cq.Length)?cq[i+1]:"?";
                    }
                    ProfileText.Text +=
                        $" | Hybrid shadow: {V("flagged_pct")}% flagged • A/B recall {V("ab_recall_pct")}% ({V("ab_covered")}/{V("ab_total")})";
                    if(!string.IsNullOrWhiteSpace(coarseDiagnosticPath))
                        ProfileText.Text += " • diagnostic CSV saved";
                    if(!string.IsNullOrWhiteSpace(fastPathSummary))
                    {
                        var fq=fastPathSummary.Split(' ',StringSplitOptions.RemoveEmptyEntries);
                        string FV(string key)
                        {
                            int i=Array.IndexOf(fq,key);
                            return (i>=0 && i+1<fq.Length)?fq[i+1]:"?";
                        }
                        ProfileText.Text +=
                            $" | Fast-path ACTIVE: {FV("eligible_pct")}% bypass • A protected by validated gate";
                    }
                    if(!string.IsNullOrWhiteSpace(blockSyncSummary))
                    {
                        var bq=blockSyncSummary.Split(' ',StringSplitOptions.RemoveEmptyEntries);
                        string BV(string key)
                        {
                            int i=Array.IndexOf(bq,key);
                            return (i>=0 && i+1<bq.Length)?bq[i+1]:"?";
                        }
                        ProfileText.Text +=
                            $" | Exact block-sync + feature cache: reacquire CPU {BV("reacquire_cpu_sec")}s/{BV("reacquire_jobs")} jobs • estimate CPU {BV("estimate_cpu_sec")}s/{BV("estimate_jobs")} jobs • wait {BV("wait_sec")}s • launch {BV("launch_sec")}s • post {BV("post_sec")}s";
                    }
                    if(!string.IsNullOrWhiteSpace(reconOuterSummary))
                    {
                        var oq=reconOuterSummary.Split(' ',StringSplitOptions.RemoveEmptyEntries);
                        string OV(string key)
                        {
                            int i=Array.IndexOf(oq,key);
                            return (i>=0 && i+1<oq.Length)?oq[i+1]:"?";
                        }
                        ProfileText.Text +=
                            $" | Reconstruction outer: block-sync {OV("block_sync_sec")}s • block-score {OV("block_score_sec")}s • segment-loop {OV("segment_loop_sec")}s";
                    }
                    if(!string.IsNullOrWhiteSpace(reconProfileSummary))
                    {
                        var rq=reconProfileSummary.Split(' ',StringSplitOptions.RemoveEmptyEntries);
                        string RV(string key)
                        {
                            int i=Array.IndexOf(rq,key);
                            return (i>=0 && i+1<rq.Length)?rq[i+1]:"?";
                        }
                        ProfileText.Text +=
                            $" | Reconstruction profiler: gate {RV("gate_sec")}s • full analysis {RV("full_analysis_sec")}s • tail {RV("tail_sec")}s";
                    }
                    if(!string.IsNullOrWhiteSpace(timeMapTotalSummary))
                    {
                        var tq=timeMapTotalSummary.Split(' ',StringSplitOptions.RemoveEmptyEntries);
                        string TV(string key)
                        {
                            int i=Array.IndexOf(tq,key);
                            return (i>=0 && i+1<tq.Length)?tq[i+1]:"?";
                        }
                        ProfileText.Text +=
                            $" | Hierarchical time-map: coarse {TV("coarse_sec")}s/{TV("coarse_calls")} calls • fine {TV("fine_sec")}s/{TV("fine_calls")} calls • other {TV("other_sec")}s";
                    }
                }

                try
                {
                    var metadata=new Dictionary<string,string>
                    {
                        ["Label"]=LabelBox.Text.Trim(),
                        ["Catalogue"]=CatalogBox.Text.Trim(),
                        ["Side"]=SideBox.Text.Trim(),
                        ["Matrix"]=MatrixBox.Text.Trim(),
                        ["Artist"]=ArtistBox.Text.Trim(),
                        ["Title"]=TitleBox.Text.Trim(),
                        ["Release year"]=ReleaseYearBox.Text.Trim(),
                        ["Recording year"]=RecordingYearBox.Text.Trim(),
                        ["Country"]=CountryBox.Text.Trim(),
                        ["Keep file speed"]=KeepFileSpeed.IsChecked==true ? "Yes — no output speed conversion" : "No",
                        ["Recorded RPM"]=(RecordedRpm.SelectedItem as ComboBoxItem)?.Content?.ToString() ?? "",
                        ["Disc nominal RPM"]=(DiscRpm.SelectedItem as ComboBoxItem)?.Content?.ToString() ?? "",
                        ["Sync search hints (seconds; automatic or manually adjusted)"]=string.Join(",",sync.Offsets.Select(x=>double.IsNaN(x)?"auto":x.ToString("R",CultureInfo.InvariantCulture))),
                        ["Master selection mode"]=sync.WasAutomatic ? "Auto — frozen after sync-aware preview" : "Manual",
                        ["Channel mode"]=stereoMode ? "Stereo" : "Mono",
                        ["Applied speed factor"]=_outputSpeedFactor.ToString("0.########",CultureInfo.InvariantCulture)
                    };
                    WriteProvenanceFile(
                        _lastOutput,
                        _lastMap,
                        _sources.Select(src=>src.OriginalPath).ToList(),
                        elapsed,
                        profileTimes,
                        metadata);
                }
                catch(Exception hashEx)
                {
                    // Auxiliary metadata must never change or invalidate a completed restoration.
                    lock(errorLines) errorLines.Add("PROVENANCE warning: "+hashEx.Message);
                }

                string speedMessage=Math.Abs(_outputSpeedFactor-1.0)>1e-9
                    ? $" Final WAV speed corrected by {_outputSpeedFactor:0.####}×."
                    : "";
                if(noSafeRepairs)
                {
                    ResultInfo.Title="Inga säkra reparationer – Master exporterad oförändrad.";
                    ResultInfo.Message=$"Ingen donatorersättning klarade samtliga kontrollsteg.{speedMessage} Diagnostik och Master-jämförelse sparades bredvid WAV-filen."+
                        (string.IsNullOrWhiteSpace(_outputQaSummary)?"":$" Output QA: {_outputQaSummary}.");
                    ResultInfo.Severity=InfoBarSeverity.Warning;
                }
                else
                {
                    bool qaWarning=_outputQaSummary.StartsWith("WARNING",StringComparison.OrdinalIgnoreCase);
                    ResultInfo.Title=qaWarning
                        ? "Restoration complete — inspect output QA"
                        : "Restoration complete";
                    ResultInfo.Message=$"Built from {_sources.Count} sources. {(stereoMode ? "Stereo: left and right preserved." : "Mono: groove channels fused.")}{speedMessage} Master-only comparison, repair log and decision map saved alongside the WAV."+
                        (string.IsNullOrWhiteSpace(_outputQaSummary)?"":$" Output QA: {_outputQaSummary}.");
                    ResultInfo.Severity=qaWarning
                        ? InfoBarSeverity.Warning : InfoBarSeverity.Success;
                }
                PresentRunSummary(repairSummary);
                ResultInfo.IsOpen=true;

                OpenResultButton.IsEnabled=true;
                OpenMapButton.IsEnabled=File.Exists(_lastMap);

                if(File.Exists(_lastMap))
                {
                    LoadSourceRolesForMap(_lastMap);
                    // Parsing a multi-minute decision CSV is substantial work. Keep it
                    // off the UI thread, and never let an optional result visualization
                    // invalidate audio that the engine has already completed and saved.
                    try
                    {
                        var loadedMap=await Task.Run(() =>
                        {
                            var map=DecisionMapData.Load(_lastMap);
                            map.PrepareForDisplay();
                            return map;
                        });
                        _mapData=loadedMap;
                        _liveMapData=null;
                        DonorWaveformScroll.ChangeView(null,0.0,null,true);
                        UpdateTimelineWidth(loadedMap);
                        DrawCurrentTimeline(loadedMap);
                        NoAnalysisHint.Visibility=Visibility.Collapsed;
                        _waveformCenterSec=loadedMap.FindInterestingTime(-0.001,1);
                        DrawWaveforms();
                        bool hasRepairs=loadedMap.ConfidenceClass.Any(c=>c is 'A' or 'B');
                        PreviousRepairButton.IsEnabled=hasRepairs;
                        NextRepairButton.IsEnabled=hasRepairs;
                        UpdateCorrectionControls();
                    }
                    catch(Exception viewEx)
                    {
                        _mapData=null;
                        _liveMapData=null;
                        PreviousRepairButton.IsEnabled=false;
                        NextRepairButton.IsEnabled=false;
                        UpdateCorrectionControls();
                        DecisionMapData.Clear(DriftLine,DriftCanvas);
                        NoAnalysisHint.Visibility=Visibility.Visible;
                        ResultInfo.Message += " Result visualization could not be loaded, but the completed WAV and CSV files are intact.";
                        lock(errorLines) errorLines.Add("RESULT VIEW warning: "+viewEx.Message);
                    }
                }
            }
            else
            {
                string err;
                if(!string.IsNullOrWhiteSpace(engineError)) err=engineError;
                else lock(errorLines) err=string.Join(Environment.NewLine,errorLines);
                if(string.IsNullOrWhiteSpace(err)) err=stdOutTask;
                if(string.IsNullOrWhiteSpace(err)) err=$"Engine exited with code {rc}.";
                ShowFailure(err);
            }
        }
        catch(Exception ex)
        {
            _analysisWatch.Stop();
            StopAnalysisHeartbeat();
            _engineProcess?.Dispose();
            _engineProcess=null;
            SetBusy(false);
            AnalysisProgress.IsIndeterminate=false;
            ShowFailure(ex.Message);
        }
    }

    private void Cancel_Click(object sender,RoutedEventArgs e)
    {
        try{if(_engineProcess is not null && !_engineProcess.HasExited){_engineProcess.Kill(true);StatusText.Text="Cancelled";DetailText.Text="Processing stopped.";PositionText.Text="";}}
        catch{} finally{_engineProcess?.Dispose();_engineProcess=null;SetBusy(false);_analysisWatch.Stop();StopAnalysisHeartbeat();AnalysisProgress.IsIndeterminate=false;ProgressText.Text="";}
    }

    private void SetBusy(bool busy)
    {
        CancelButton.IsEnabled=busy;
        SourcesList.IsEnabled=!busy;
        ChannelModeToggle.IsEnabled=!busy;
        ForensicButton.IsEnabled=!busy;
        MasterMode.IsEnabled=!busy;
        KeepFileSpeed.IsEnabled=!busy;
        RecordedRpm.IsEnabled=!busy && KeepFileSpeed.IsChecked!=true;
        DiscRpm.IsEnabled=!busy && KeepFileSpeed.IsChecked!=true;
        if(!busy) UpdateSourceState(); else AnalyzeButton.IsEnabled=false;
        UpdateCorrectionControls();
    }

    private static string Sha256File(string path)
    {
        using var sha=System.Security.Cryptography.SHA256.Create();
        using var fs=System.IO.File.OpenRead(path);
        return Convert.ToHexString(sha.ComputeHash(fs)).ToLowerInvariant();
    }

    private static void WriteProvenanceFile(
        string outputWav,
        string decisionCsv,
        IReadOnlyList<string> inputs,
        TimeSpan elapsed,
        System.Collections.Concurrent.ConcurrentDictionary<string,double> profileTimes,
        IReadOnlyDictionary<string,string> metadata)
    {
        static string Hms(double seconds)
        {
            var ts=TimeSpan.FromSeconds(Math.Max(0,seconds));
            return $"{(int)ts.TotalHours:00}:{ts.Minutes:00}:{ts.Seconds:00}";
        }

        string prov=System.IO.Path.ChangeExtension(outputWav,".provenance.sha256.txt");
        using var w=new System.IO.StreamWriter(prov,false,new System.Text.UTF8Encoding(false));
        w.WriteLine("MASTER-2000 PROVENANCE / REGRESSION RECORD");
        w.WriteLine("Engine version: v0.3.3");
        w.WriteLine($"Elapsed: {(int)elapsed.TotalHours:00}:{elapsed.Minutes:00}:{elapsed.Seconds:00}");
        foreach(var key in new[]{"master_preflight","global_alignment","verified_time_maps","reconstruction","write_output","total"})
            if(profileTimes.TryGetValue(key,out double v))
                w.WriteLine($"Profile {key}: {Hms(v)}");

        w.WriteLine();
        w.WriteLine("RECORD METADATA");
        foreach(var kv in metadata)
            w.WriteLine($"{kv.Key}: {kv.Value}");
        w.WriteLine();

        for(int i=0;i<inputs.Count;i++)
            w.WriteLine($"INPUT {i+1} SHA-256  {Sha256File(inputs[i])}  {System.IO.Path.GetFileName(inputs[i])}");

        if(System.IO.File.Exists(outputWav))
            w.WriteLine($"OUTPUT WAV SHA-256  {Sha256File(outputWav)}  {System.IO.Path.GetFileName(outputWav)}");

        if(System.IO.File.Exists(decisionCsv))
            w.WriteLine($"DECISIONS SHA-256  {Sha256File(decisionCsv)}  {System.IO.Path.GetFileName(decisionCsv)}");

        string stem=System.IO.Path.Combine(System.IO.Path.GetDirectoryName(outputWav)!,
            System.IO.Path.GetFileNameWithoutExtension(outputWav));
        foreach(var sidecar in new[]{
            (Label:"MASTER-ONLY",Path:stem+".master-only.wav"),
            (Label:"REPAIRS",Path:stem+".repairs.csv"),
            (Label:"OUTPUT-QA",Path:stem+".output-qa.csv"),
            (Label:"SOURCE-ROLES",Path:stem+".source-roles.csv")})
            if(System.IO.File.Exists(sidecar.Path))
                w.WriteLine($"{sidecar.Label} SHA-256  {Sha256File(sidecar.Path)}  {System.IO.Path.GetFileName(sidecar.Path)}");
    }

    private const string LicenseText =
@"MIT License

Copyright (c) 2000–2026 Olaf Hallberg

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the ""Software""), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED ""AS IS"", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

Keep independent backups of original recordings. Processing occurs locally; optional record lookup is initiated by the user and does not upload audio.";

    private async Task<bool> ShowLicenseDialogAsync()
    {
        var dialog=new ContentDialog { XamlRoot=Content.XamlRoot, Title="Master-2000 — MIT License", CloseButtonText="Close", Content=new ScrollViewer { MaxHeight=430, Content=new TextBlock { Text=LicenseText, TextWrapping=TextWrapping.Wrap } } };
        await dialog.ShowAsync();
        return true;
    }

    private async void License_Click(object sender,RoutedEventArgs e)
    {
        await ShowLicenseDialogAsync();
    }

    private async void Help_Click(object sender,RoutedEventArgs e)
    {
        var dialog=new ContentDialog
        {
            XamlRoot=Content.XamlRoot,
            Title="Master-2000 — Quick Start",
            CloseButtonText="Close",
            Content=new ScrollViewer
            {
                MaxHeight=560,
                Content=new TextBlock
                {
                    TextWrapping=TextWrapping.Wrap,
                    Text=
@"QUICK START
1. Record three to six different physical copies of the same pressing.
2. Use the same equipment and settings for every copy.
3. Add the lossless audio files to Master-2000.
4. Enter the record information if known.
5. Click Analyze & restore.
6. Listen to the restored WAV and keep the provenance files.

BEST RESULTS
• Use 96 kHz / 24-bit PCM WAV for new recordings.
• Use the correct stylus for the record type.
• Clean records carefully. Never use alcohol, alcohol-based cleaners or other solvents on shellac/78 rpm records. If the material is unknown, do not use a solvent.
• Do not denoise, de-click, EQ, normalize, compress or otherwise digitally process the source recordings before Master-2000.
• Keep the original raw audio files unchanged.
• Supported imports: PCM WAV/WAVE, FLAC, AIFF/AIFC, ALAC/M4A and WMA Lossless.
• Load previous run restores saved decisions and curves without rerunning analysis.
• More independent copies can improve reconstruction.

RPM
Keep file speed is enabled by default and disables output speed conversion. Uncheck it to convert from File current RPM to Target RPM after reconstruction. Local source synchronization still operates.

COMPUTER
Master-2000 is CPU-intensive. Prefer fast CPU cores first, then more cores/threads. 16 GB RAM is recommended; 32 GB or more is preferable for long 96 kHz / 24-bit recordings. Use an SSD. A powerful GPU is not required.

ONLINE LOOKUP
Online record lookup is optional and happens only when you press Look up record. Only the record-identification text you entered is used for the search. Audio files, hashes and provenance are never uploaded by Master-2000.

RIGHTS
Online identification is not legal clearance. Composition and recording rights may be separate.

A/B/C MAP
A = verified repair using an authentic donor.
B = suspicious/probable event, but Master was kept.
C = unchanged Master.
Master-2000 synthesizes no replacement audio."
                }
            }
        };
        await dialog.ShowAsync();
    }

    private async void About_Click(object sender,RoutedEventArgs e)
    {
        var panel=new StackPanel { Spacing=8 };
        panel.Children.Add(new TextBlock
        {
            TextWrapping=TextWrapping.Wrap,
            Text="Master-2000 v0.3.3\nMulti-source archival audio reconstruction\n\nRecorded sound — not guesses.\n\n© 2000–2026 Olaf Hallberg\n\nLocal processing only. No cloud processing and no generated replacement audio."
        });
        panel.Children.Add(new TextBlock { Text="Project contact:", FontWeight=Microsoft.UI.Text.FontWeights.SemiBold, Margin=new Thickness(0,8,0,0) });
        panel.Children.Add(new HyperlinkButton
        {
            Content=ContactEmail,
            NavigateUri=new Uri("mailto:"+ContactEmail+"?subject="+Uri.EscapeDataString("Master-2000 v0.3.3")),
            Padding=new Thickness(0)
        });
        panel.Children.Add(new TextBlock { Text="Use the License button to review the license and disclaimer.", Opacity=0.65, TextWrapping=TextWrapping.Wrap });

        var dialog=new ContentDialog
        {
            XamlRoot=Content.XamlRoot,
            Title="About Master-2000",
            CloseButtonText="Close",
            Content=panel
        };
        await dialog.ShowAsync();
    }

    private void LookupRecord_Click(object sender,RoutedEventArgs e)
    {
        var parts=new[]{LabelBox.Text,CatalogBox.Text,ArtistBox.Text,TitleBox.Text,MatrixBox.Text,ReleaseYearBox.Text,RecordingYearBox.Text,CountryBox.Text}
            .Where(v=>!string.IsNullOrWhiteSpace(v));
        string q=string.Join(" ",parts);
        if(string.IsNullOrWhiteSpace(q))
        {
            ResultInfo.Severity=InfoBarSeverity.Informational;
            ResultInfo.Title="Enter record details first";
            ResultInfo.Message="Label + catalogue number is a good starting point.";
            ResultInfo.IsOpen=true;
            return;
        }
        string url="https://www.discogs.com/search/?q="+Uri.EscapeDataString(q)+"&type=release";
        Process.Start(new ProcessStartInfo(url){UseShellExecute=true});
    }

    private void ShowFailure(string message){StatusText.Text="Processing failed";DetailText.Text=message.Trim();PositionText.Text="";ProfileText.Text="";ProgressText.Text="";ResultInfo.Title="Processing failed";ResultInfo.Message=message.Trim();ResultInfo.Severity=InfoBarSeverity.Error;ResultInfo.IsOpen=true;}
    private async void LoadPreviousRun_Click(object sender,RoutedEventArgs e)
    {
        try
        {
            var guide=new ContentDialog
            {
                XamlRoot=Content.XamlRoot,Title="Load a previous run",
                PrimaryButtonText="Choose decisions CSV",CloseButtonText="Cancel",
                Content=new TextBlock
                {
                    Text="1. Choose the run’s .decisions.csv (not repairs or diagnostic CSV).\n\n2. If requested, choose the original restored Result WAV from that run, normally Master-2000-restored.wav.\n\n3. If requested, choose the original input recordings.\n\nKeep the companion logs and provenance file in the same folder. Separately corrected or start-blanked copies do not match the original repair map.",
                    TextWrapping=TextWrapping.Wrap,MaxWidth=500
                }
            };
            if(await guide.ShowAsync()!=ContentDialogResult.Primary)return;
            var mapPicker=new FileOpenPicker();
            WinRT.Interop.InitializeWithWindow.Initialize(mapPicker,Hwnd);
            mapPicker.FileTypeFilter.Add(".csv");
            var mapFile=await mapPicker.PickSingleFileAsync();
            if(mapFile is null) return;

            string mapPath=mapFile.Path;
            string suffix=".decisions.csv";
            string stem=mapPath.EndsWith(suffix,StringComparison.OrdinalIgnoreCase)
                ? mapPath[..^suffix.Length]
                : System.IO.Path.Combine(System.IO.Path.GetDirectoryName(mapPath)!,
                    System.IO.Path.GetFileNameWithoutExtension(mapPath));
            string outputPath=stem+".wav";
            string provenancePath=stem+".provenance.sha256.txt";

            if(!File.Exists(outputPath))
            {
                var resultPicker=new FileOpenPicker();
                WinRT.Interop.InitializeWithWindow.Initialize(resultPicker,Hwnd);
                resultPicker.FileTypeFilter.Add(".wav");
                var resultFile=await resultPicker.PickSingleFileAsync();
                if(resultFile is null) return;
                outputPath=resultFile.Path;
                string candidate=System.IO.Path.ChangeExtension(outputPath,".provenance.sha256.txt");
                if(File.Exists(candidate)) provenancePath=candidate;
            }

            var expectedHashes=ReadProvenanceSourceHashes(provenancePath);
            bool sourcesMatch=expectedHashes.Count>=3 && expectedHashes.All(hash=>
                _sourceHashes.ContainsKey(hash));
            if(!sourcesMatch)
            {
                var sourcePicker=new FileOpenPicker();
                WinRT.Interop.InitializeWithWindow.Initialize(sourcePicker,Hwnd);
                foreach(string ext in LosslessAudio.Extensions) sourcePicker.FileTypeFilter.Add(ext);
                var sourceFiles=await sourcePicker.PickMultipleFilesAsync();
                if(sourceFiles.Count<3)
                    throw new InvalidDataException("Select the original source files used by this run.");
                _sources.Clear();
                _sourceHashes.Clear();
                foreach(var sourceFile in sourceFiles) await AddSourceAsync(sourceFile.Path);
            }

            if(expectedHashes.Count>=3)
            {
                var ordered=new List<SourceItem>();
                foreach(string hash in expectedHashes)
                {
                    if(!_sourceHashes.TryGetValue(hash,out string? originalPath))
                        throw new InvalidDataException("A selected source does not match the run's provenance hashes.");
                    var source=_sources.FirstOrDefault(s=>string.Equals(
                        s.OriginalPath,originalPath,StringComparison.OrdinalIgnoreCase));
                    if(source is null)
                        throw new InvalidDataException("Could not restore the original source order.");
                    ordered.Add(source);
                }
                _sources.Clear();
                foreach(var source in ordered) _sources.Add(source);
                Renumber();
                RebuildSourceHashes();
                UpdateSourceState();
            }
            else if(_sources.Count<3)
                throw new InvalidDataException("This old run has no usable provenance file; select its sources in their original order.");

            var loadedMap=await Task.Run(() =>
            {
                var map=DecisionMapData.Load(mapPath);
                map.PrepareForDisplay();
                return map;
            });
            if(loadedMap.TimeSec.Count==0)
                throw new InvalidDataException("The selected CSV is not a Master-2000 decision map.");

            _lastMap=mapPath;
            _lastOutput=outputPath;
            _revertedRepairs.Clear();
            _outputQaSummary=ReadOutputQaSummary(outputPath);
            LoadSourceRolesForMap(mapPath);
            _mapData=loadedMap;
            _liveMapData=null;
            DonorWaveformScroll.ChangeView(null,0.0,null,true);
            _timelineDrawingDisabled=false;
            _timelineUserScrolled=false;
            _outputSpeedFactor=ReadProvenanceSpeedFactor(provenancePath);
            UpdateTimelineWidth(loadedMap);
            DrawCurrentTimeline(loadedMap);
            NoAnalysisHint.Visibility=Visibility.Collapsed;
            _waveformCenterSec=loadedMap.FindInterestingTime(-0.001,1);
            DrawWaveforms();
            OpenResultButton.IsEnabled=true;
            OpenMapButton.IsEnabled=true;
            bool hasRepairs=loadedMap.ConfidenceClass.Any(c=>c is 'A' or 'B');
            PreviousRepairButton.IsEnabled=hasRepairs;
            NextRepairButton.IsEnabled=hasRepairs;
            UpdateCorrectionControls();
            AnalysisProgress.IsIndeterminate=false;
            AnalysisProgress.Value=100;
            ProgressText.Text="Loaded previous run";
            StatusText.Text="Previous run loaded";
            DetailText.Text=System.IO.Path.GetFileName(outputPath);
            PositionText.Text=$"{loadedMap.TimeSec.Count:N0} saved decisions • sources restored from provenance";
            ProfileText.Text="No analysis was rerun; curves and overlays come from the saved result.";
            bool qaWarning=_outputQaSummary.StartsWith("WARNING",StringComparison.OrdinalIgnoreCase);
            ResultInfo.Severity=qaWarning
                ? InfoBarSeverity.Warning : InfoBarSeverity.Success;
            ResultInfo.Title=qaWarning
                ? "Previous restoration loaded — inspect output QA"
                : "Previous restoration loaded";
            ResultInfo.Message="Decision map, source order, aligned waveforms and repair overlay restored. Recording mode switch applies to the next run."+
                (string.IsNullOrWhiteSpace(_outputQaSummary)?"":$" Output QA: {_outputQaSummary}.");
            PresentRunSummary(ReadRepairRunSummary(mapPath));
            ResultInfo.IsOpen=true;
        }
        catch(Exception ex)
        {
            HideRunSummary();
            ResultInfo.Severity=InfoBarSeverity.Error;
            ResultInfo.Title="Could not load previous run";
            ResultInfo.Message=ex.Message;
            ResultInfo.IsOpen=true;
        }
    }

    private static List<string> ReadProvenanceSourceHashes(string path)
    {
        var indexed=new SortedDictionary<int,string>();
        if(!File.Exists(path)) return [];
        foreach(string line in File.ReadLines(path))
        {
            if(!line.StartsWith("INPUT ",StringComparison.Ordinal)) continue;
            string[] p=line.Split(' ',StringSplitOptions.RemoveEmptyEntries);
            if(p.Length>=4 && int.TryParse(p[1],out int index) &&
               p[2].Equals("SHA-256",StringComparison.OrdinalIgnoreCase) &&
               p[3].Length==64)
                indexed[index]=p[3].ToLowerInvariant();
        }
        return indexed.Values.ToList();
    }

    private void LoadSourceRolesForMap(string mapPath)
    {
        _sourceRoles.Clear();
        const string suffix=".decisions.csv";
        string rolePath=mapPath.EndsWith(suffix,StringComparison.OrdinalIgnoreCase)
            ? mapPath[..^suffix.Length]+".source-roles.csv"
            : mapPath+".source-roles.csv";
        if(!File.Exists(rolePath)) return;
        foreach(string line in File.ReadLines(rolePath).Skip(1))
        {
            string[] fields=line.Split(',');
            if(fields.Length>=2 && int.TryParse(fields[0],out int source))
                _sourceRoles[source]=fields[1].Trim();
        }
    }

    private static double ReadProvenanceSpeedFactor(string path)
    {
        if(File.Exists(path))
        {
            const string prefix="Applied speed factor:";
            foreach(string line in File.ReadLines(path))
                if(line.StartsWith(prefix,StringComparison.OrdinalIgnoreCase) &&
                   double.TryParse(line[prefix.Length..].Trim(),NumberStyles.Float,
                       CultureInfo.InvariantCulture,out double factor) &&
                   factor>0.0 && double.IsFinite(factor)) return factor;
        }
        return 1.0;
    }

    private static string FormatOutputQa(string line)
    {
        if(string.IsNullOrWhiteSpace(line)) return "";
        string[] fields=line.Split(' ',StringSplitOptions.RemoveEmptyEntries);
        string V(string key)
        {
            int index=Array.IndexOf(fields,key);
            return index>=0 && index+1<fields.Length ? fields[index+1] : "?";
        }
        string status=V("status");
        string clipped=V("clipped_samples");
        string baseline=V("baseline_clipped_samples");
        long.TryParse(clipped,out long clippedCount);
        long.TryParse(baseline,out long baselineCount);
        long introduced=Math.Max(0,clippedCount-baselineCount);
        return $"{status}; peak {V("peak")}; new clipped samples {introduced}; boundary flags {V("seam_flags")}; DC shift {V("dc_shift")}";
    }

    private static string ReadOutputQaSummary(string resultPath)
    {
        string path=Path.Combine(Path.GetDirectoryName(resultPath)!,
            Path.GetFileNameWithoutExtension(resultPath)+".output-qa.csv");
        if(!File.Exists(path)) return "";
        var values=new Dictionary<string,string>(StringComparer.OrdinalIgnoreCase);
        foreach(string line in File.ReadLines(path).Skip(1))
        {
            int comma=line.IndexOf(',');
            if(comma>0) values[line[..comma].Trim()]=line[(comma+1)..].Trim();
        }
        string V(string key)=>values.TryGetValue(key,out string? value)?value:"?";
        long.TryParse(V("clipped_samples"),out long clipped);
        long.TryParse(V("baseline_clipped_samples"),out long baseline);
        return $"{V("status")}; peak {V("peak")}; new clipped samples {Math.Max(0,clipped-baseline)}; boundary flags {V("repair_boundary_flags")}; DC shift {V("max_abs_dc_shift_from_master")}";
    }

    private void UpdateCorrectionControls()
    {
        if(ReviewFlaggedButton is null) return;
        bool idle=_engineProcess is null;
        bool ready=idle && _mapData is not null && _lastOutput is not null &&
            File.Exists(_lastOutput);
        string baseline=_lastOutput is null ? "" : UserCorrection.MasterOnlyPath(_lastOutput);
        bool hasBaseline=ready && File.Exists(baseline);
        var current=_mapData?.RepairIntervalAt(_waveformCenterSec);
        bool currentReverted=current is { } repair && _revertedRepairs.Contains(repair);
        if(TrySourceButton is not null)TrySourceButton.IsEnabled=ready && _sources.Count>=3 && _sources.Count<=6;
        ReviewFlaggedButton.IsEnabled=ready && (_mapData?.FlaggedIntervalCount ?? 0)>0;
        RevertRepairButton.IsEnabled=hasBaseline && current is not null && !currentReverted;
        RestoreRepairButton.IsEnabled=ready && current is not null && currentReverted;
        SaveCorrectedButton.IsEnabled=hasBaseline && _revertedRepairs.Count>0;
        PlayMasterResultButton.IsEnabled=hasBaseline;
        PlayResultCursorButton.IsEnabled=ready;
        if(!ready)
        {
            CorrectionStatusText.Text="";
            return;
        }
        string currentStatus=_mapData!.ConfidenceAt(_waveformCenterSec)=='B'
            ? $"B: {_mapData.DecisionReasonAt(_waveformCenterSec)}"
            : currentReverted ? "Current A repair will use Master" : "";
        string total=_revertedRepairs.Count>0
            ? $"{_revertedRepairs.Count} repair(s) undone"
            : $"{_mapData.FlaggedIntervalCount} B area(s) kept as Master";
        CorrectionStatusText.Text=string.IsNullOrWhiteSpace(currentStatus)
            ? total : total+" • "+currentStatus;
    }

    private void OpenResult_Click(object sender,RoutedEventArgs e){if(_lastOutput is not null&&File.Exists(_lastOutput))Process.Start(new ProcessStartInfo(_lastOutput){UseShellExecute=true});}
    private void OpenMap_Click(object sender,RoutedEventArgs e){if(_lastMap is not null&&File.Exists(_lastMap))Process.Start(new ProcessStartInfo(_lastMap){UseShellExecute=true});}
    private double VisibleTimelineSeconds() => TimelineZoom.SelectedIndex switch
    {
        0 => 5.0, 1 => 10.0, 2 => 30.0, 3 => 60.0, _ => double.PositiveInfinity
    };

    private void UpdateTimelineWidth(DecisionMapData? data)
    {
        double viewport=Math.Max(400.0, TimelineScroll.ActualWidth);
        if(data is null || data.DurationSec<=0 || double.IsInfinity(VisibleTimelineSeconds()))
        {
            DriftCanvas.Width=viewport;
            return;
        }
        double visible=VisibleTimelineSeconds();
        DriftCanvas.Width=Math.Max(viewport, viewport*data.DurationSec/visible);
    }

    private void TimelineZoom_SelectionChanged(object sender,SelectionChangedEventArgs e)
    {
        var data=_mapData ?? _liveMapData;
        if(TimelineScroll is null || DriftCanvas is null) return;
        UpdateTimelineWidth(data);
        if(data is not null)
        {
            DrawCurrentTimeline(data);
            UpdateWaveformViewportIndicator(keepVisible:true);
        }
    }

    private void TimelineScroll_ViewChanged(object sender,ScrollViewerViewChangedEventArgs e)
    {
        if(e.IsIntermediate && TimelineScroll.HorizontalOffset < TimelineScroll.ScrollableWidth-4)
            _timelineUserScrolled=true;
        if(!e.IsIntermediate)
        {
            var data=_mapData ?? _liveMapData;
            if(data is not null) DrawCurrentTimeline(data);
        }
    }

    private void DrawCurrentTimeline(DecisionMapData data)
    {
        if(_timelineDrawingDisabled) return;
        // Draw only what the ScrollViewer can currently show. The backing timeline
        // remains full length, but UI object count no longer grows with song length.
        try
        {
            data.DrawViewport(DriftLine, DriftCanvas, TimelineScroll.HorizontalOffset, TimelineScroll.ActualWidth);
            UpdateWaveformViewportIndicator(keepVisible:false);
        }
        catch(Exception ex)
        {
            _timelineDrawingDisabled=true;
            DecisionMapData.Clear(DriftLine,DriftCanvas);
            NoAnalysisHint.Visibility=Visibility.Visible;
            ResultInfo.Severity=InfoBarSeverity.Warning;
            ResultInfo.Title="Result view unavailable";
            ResultInfo.Message="The graph could not be displayed, but completed WAV and CSV files remain intact. "+ex.Message;
            ResultInfo.IsOpen=true;
        }
    }

    private void DriftCanvas_SizeChanged(object sender,SizeChangedEventArgs e)
    {
        var data=_mapData ?? _liveMapData;
        if(data is not null) DrawCurrentTimeline(data);
    }

    private double WaveformSpanSeconds() => WaveformZoom.SelectedIndex switch
    {
        0 => 0.500, 1 => 0.100, 2 => 0.020, _ => 0.005
    };

    private void UpdateWaveformViewportIndicator(bool keepVisible)
    {
        if(_mapData is null || _mapData.DurationSec<=0 || DriftCanvas.ActualWidth<=1)
        {
            WaveformViewportLine.Visibility=Visibility.Collapsed;
            return;
        }

        double width=DriftCanvas.ActualWidth;
        double duration=_mapData.DurationSec;
        double half=WaveformSpanSeconds()*0.5;
        double start=Math.Clamp(_waveformCenterSec-half,0.0,duration);
        double end=Math.Clamp(_waveformCenterSec+half,0.0,duration);
        double x1=width*start/duration;
        double x2=width*end/duration;
        if(x2-x1<4.0)
        {
            double middle=0.5*(x1+x2);
            x1=Math.Max(0.0,middle-2.0);
            x2=Math.Min(width,middle+2.0);
        }
        double y=Math.Max(4.0,DriftCanvas.ActualHeight-5.0);
        WaveformViewportLine.X1=x1;
        WaveformViewportLine.X2=x2;
        WaveformViewportLine.Y1=y;
        WaveformViewportLine.Y2=y;
        WaveformViewportLine.Visibility=Visibility.Visible;

        if(!keepVisible || TimelineScroll.ActualWidth<=1) return;
        double center=0.5*(x1+x2);
        double viewLeft=TimelineScroll.HorizontalOffset;
        double viewWidth=TimelineScroll.ActualWidth;
        double margin=Math.Min(80.0,viewWidth*0.15);
        if(center<viewLeft+margin || center>viewLeft+viewWidth-margin)
        {
            double target=Math.Clamp(center-viewWidth*0.5,0.0,Math.Max(0.0,width-viewWidth));
            TimelineScroll.ChangeView(target,null,null,true);
        }
    }

    private void DriftCanvas_PointerPressed(object sender,PointerRoutedEventArgs e)
    {
        if(_mapData is null || _mapData.DurationSec<=0 || DriftCanvas.ActualWidth<=1) return;
        var p=e.GetCurrentPoint(DriftCanvas).Position;
        _waveformCenterSec=Math.Clamp(p.X/DriftCanvas.ActualWidth*_mapData.DurationSec,0.0,_mapData.DurationSec);
        DrawWaveforms();
    }

    private void PreviousRepair_Click(object sender,RoutedEventArgs e)
    {
        if(_mapData is null) return;
        _waveformCenterSec=_mapData.FindInterestingTime(_waveformCenterSec-0.000001,-1);
        DrawWaveforms();
    }

    private void NextRepair_Click(object sender,RoutedEventArgs e)
    {
        if(_mapData is null) return;
        _waveformCenterSec=_mapData.FindInterestingTime(_waveformCenterSec+0.000001,1);
        DrawWaveforms();
    }

    private void ReviewFlagged_Click(object sender,RoutedEventArgs e)
    {
        if(_mapData is null || _mapData.FlaggedIntervalCount==0) return;
        _waveformCenterSec=_mapData.FindClassTime(
            _waveformCenterSec+0.000001,'B',1,true);
        DrawWaveforms();
    }

    private void RevertRepair_Click(object sender,RoutedEventArgs e)
    {
        if(_mapData?.RepairIntervalAt(_waveformCenterSec) is not { } repair) return;
        _revertedRepairs.Add(repair);
        DrawWaveforms();
    }

    private void RestoreRepair_Click(object sender,RoutedEventArgs e)
    {
        if(_mapData?.RepairIntervalAt(_waveformCenterSec) is not { } repair) return;
        _revertedRepairs.Remove(repair);
        DrawWaveforms();
    }

    private async void SaveCorrected_Click(object sender,RoutedEventArgs e)
    {
        if(_lastOutput is null || _revertedRepairs.Count==0) return;
        string baseline=UserCorrection.MasterOnlyPath(_lastOutput);
        if(!File.Exists(baseline))
        {
            ResultInfo.Severity=InfoBarSeverity.Error;
            ResultInfo.Title="Master comparison missing";
            ResultInfo.Message="The master-only WAV must be beside the result before repairs can be undone.";
            ResultInfo.IsOpen=true;
            return;
        }
        var picker=new FileSavePicker();
        WinRT.Interop.InitializeWithWindow.Initialize(picker,Hwnd);
        picker.SuggestedFileName=Path.GetFileNameWithoutExtension(_lastOutput)+"-user-corrected";
        picker.FileTypeChoices.Add("PCM WAV",new List<string>{".wav"});
        var destination=await picker.PickSaveFileAsync();
        if(destination is null) return;
        SaveCorrectedButton.IsEnabled=false;
        try
        {
            var selected=_revertedRepairs.OrderBy(repair=>repair.Start).ToList();
            long frames=await Task.Run(()=>UserCorrection.Save(
                _lastOutput,baseline,destination.Path,selected,_outputSpeedFactor));
            var format=WavInfo.Read(destination.Path);
            double milliseconds=format is null ? 0.0 : 1000.0*frames/format.SampleRate;
            ResultInfo.Severity=InfoBarSeverity.Success;
            ResultInfo.Title="User-corrected WAV saved";
            ResultInfo.Message=$"{selected.Count} accepted repair(s) were restored to exact Master audio ({milliseconds:0.###} ms). The engine result was not overwritten; an audit CSV was saved beside the new WAV.";
            ResultInfo.IsOpen=true;
        }
        catch(Exception ex)
        {
            ResultInfo.Severity=InfoBarSeverity.Error;
            ResultInfo.Title="Could not save corrected WAV";
            ResultInfo.Message=ex.Message;
            ResultInfo.IsOpen=true;
        }
        finally { UpdateCorrectionControls(); }
    }

    private void WaveformZoom_SelectionChanged(object sender,SelectionChangedEventArgs e)
    {
        if(MasterWaveformCanvas is not null) DrawWaveforms();
    }

    private void RepairOverlayToggle_Toggled(object sender,RoutedEventArgs e)
    {
        if(RepairOverlayToggle is null) return;
        _repairOverlayEnabled=RepairOverlayToggle.IsOn;
        if(_repairOverlayInitializing) return;
        try
        {
            string path=RepairOverlaySettingPath();
            Directory.CreateDirectory(System.IO.Path.GetDirectoryName(path)!);
            File.WriteAllText(path,_repairOverlayEnabled ? "1" : "0");
        }
        catch { }
        DrawWaveforms();
    }

    private void LevelMatchToggle_Toggled(object sender,RoutedEventArgs e)
    {
        if(LevelMatchToggle is null) return;
        _levelMatchEnabled=LevelMatchToggle.IsOn;
        DrawWaveforms();
    }

    private void WaveformCanvas_SizeChanged(object sender,SizeChangedEventArgs e) => DrawWaveforms();

    private void WaveformCanvas_PointerWheelChanged(object sender,PointerRoutedEventArgs e)
    {
        if(sender is not Canvas canvas || _mapData is null || canvas.ActualWidth<=1) return;
        int delta=e.GetCurrentPoint(canvas).Properties.MouseWheelDelta;
        if(delta==0) return;

        int oldIndex=WaveformZoom.SelectedIndex;
        int newIndex=Math.Clamp(oldIndex+(delta>0?1:-1),0,3);
        if(newIndex==oldIndex) { e.Handled=true; return; }

        double oldSpan=WaveformSpanSeconds();
        double plotLeft=112.0;
        double plotRight=Math.Max(plotLeft+1.0,canvas.ActualWidth-12.0);
        double x=e.GetCurrentPoint(canvas).Position.X;
        double u=Math.Clamp((x-plotLeft)/(plotRight-plotLeft),0.0,1.0);
        double timeUnderPointer=_waveformCenterSec+(u-0.5)*oldSpan;

        WaveformZoom.SelectedIndex=newIndex;
        double newSpan=WaveformSpanSeconds();
        _waveformCenterSec=Math.Clamp(timeUnderPointer-(u-0.5)*newSpan,0.0,_mapData.DurationSec);
        DrawWaveforms();
        e.Handled=true;
    }

    private void WaveformCanvas_PointerPressed(object sender,PointerRoutedEventArgs e)
    {
        if(sender is not Canvas canvas || _mapData is null || canvas.ActualWidth<=1) return;
        var point=e.GetCurrentPoint(canvas);
        if(!point.Properties.IsLeftButtonPressed) return;
        _waveformPanActive=true;
        _waveformPanPointerX=point.Position.X;
        _waveformPanStartX=point.Position.X;
        _waveformPanStartCenterSec=_waveformCenterSec;
        _waveformPanPressedTicks=Stopwatch.GetTimestamp();
        _waveformPanLastTicks=_waveformPanPressedTicks;
        _waveformPointerCanvas=canvas;
        canvas.CapturePointer(e.Pointer);
        _waveformPanTimer.Start();
        e.Handled=true;
    }

    private void WaveformCanvas_PointerMoved(object sender,PointerRoutedEventArgs e)
    {
        if(!_waveformPanActive) return;
        Canvas canvas=_waveformPointerCanvas ?? (Canvas)sender;
        _waveformPanPointerX=e.GetCurrentPoint(canvas).Position.X;
        e.Handled=true;
    }

    private void WaveformPanTimer_Tick(object? sender,object e)
    {
        Canvas canvas=_waveformPointerCanvas ?? MasterWaveformCanvas;
        if(!_waveformPanActive || _mapData is null || canvas.ActualWidth<=1) return;
        long now=Stopwatch.GetTimestamp();
        double dt=(double)(now-_waveformPanLastTicks)/Stopwatch.Frequency;
        _waveformPanLastTicks=now;

        const double plotLeft=112.0;
        double plotRight=Math.Max(plotLeft+1.0,canvas.ActualWidth-12.0);
        double signed=Math.Clamp(2.0*(_waveformPanPointerX-plotLeft)/(plotRight-plotLeft)-1.0,-1.0,1.0);
        const double deadZone=0.08;
        if(Math.Abs(signed)<=deadZone) return;
        double amount=(Math.Abs(signed)-deadZone)/(1.0-deadZone);
        double direction=Math.Sign(signed);
        double secondsPerSecond=WaveformSpanSeconds()*1.6*Math.Pow(amount,1.6);
        double next=Math.Clamp(_waveformCenterSec+direction*secondsPerSecond*dt,0.0,_mapData.DurationSec);
        if(Math.Abs(next-_waveformCenterSec)>1e-12)
        {
            _waveformCenterSec=next;
            DrawWaveforms();
        }
    }

    private void WaveformCanvas_PointerReleased(object sender,PointerRoutedEventArgs e)
    {
        if(!_waveformPanActive || _mapData is null) return;
        Canvas canvas=_waveformPointerCanvas ?? (Canvas)sender;
        var p=e.GetCurrentPoint(canvas).Position;
        double held=(double)(Stopwatch.GetTimestamp()-_waveformPanPressedTicks)/Stopwatch.Frequency;
        bool shortClick=held<0.25 && Math.Abs(p.X-_waveformPanStartX)<5.0;
        StopWaveformPan();
        canvas.ReleasePointerCapture(e.Pointer);
        if(shortClick)
        {
            double plotLeft=112.0;
            double plotRight=Math.Max(plotLeft+1.0,canvas.ActualWidth-12.0);
            double u=Math.Clamp((p.X-plotLeft)/(plotRight-plotLeft),0.0,1.0);
            _waveformCenterSec=Math.Clamp(
                _waveformPanStartCenterSec+(u-0.5)*WaveformSpanSeconds(),0.0,_mapData.DurationSec);
            DrawWaveforms();
        }
        e.Handled=true;
    }

    private void WaveformCanvas_PointerCaptureLost(object sender,PointerRoutedEventArgs e) => StopWaveformPan();

    private void StopWaveformPan()
    {
        _waveformPanActive=false;
        _waveformPanTimer.Stop();
        _waveformPointerCanvas=null;
    }

    private bool _drawingWaveforms;
    private void DrawWaveforms()
    {
        if(_drawingWaveforms)return;
        _drawingWaveforms=true;
        double? offset=AnalysisScroll?.VerticalOffset;
        try
        {
            DrawWaveformsCore();
            // Resolve layout while guarded against SizeChanged re-entry.
            // Zoom/pan changes time, never the page's vertical position.
            if(offset.HasValue && AnalysisScroll is not null)
            {
                AnalysisScroll.UpdateLayout();
                if(Math.Abs(AnalysisScroll.VerticalOffset-offset.Value)>0.1)
                    AnalysisScroll.ChangeView(null,offset.Value,null,true);
            }
        }
        finally { _drawingWaveforms=false; }
    }

    private void DrawWaveformsCore()
    {
        if(_mapData is null || _lastOutput is null || !File.Exists(_lastOutput) ||
           MasterWaveformCanvas is null || DonorWaveformCanvas is null ||
           ResultWaveformCanvas is null) return;
        int i=_mapData.ContainingIndex(_waveformCenterSec);
        char cc=i>=0 && i<_mapData.ConfidenceClass.Count ? _mapData.ConfidenceClass[i] : 'C';
        int decisionSource=i>=0 && i<_mapData.SelectedSource.Count ? _mapData.SelectedSource[i] : 1;
        int master=Math.Clamp(_mapData.MasterSource,1,Math.Max(1,_sources.Count));
        string masterName=master<=_sources.Count ? Path.GetFileName(_sources[master-1].OriginalPath) : "unknown";
        var repair=_mapData.RepairIntervalAt(_waveformCenterSec);
        bool reverted=repair is { } accepted && _revertedRepairs.Contains(accepted);
        string outputAtCursor;
        if(reverted)
            outputAtCursor=$"output at cursor: exact Master (Source {master}, user undo)";
        else if(repair is { } renderedRepair)
        {
            if(renderedRepair.HasRenderDetails)
                outputAtCursor=renderedRepair.IsCoreAt(_waveformCenterSec)
                    ? $"output at cursor: Source {renderedRepair.DonorSource} rendered core ×{renderedRepair.RenderGain:0.###}, DC {renderedRepair.RenderDc:+0.000000;-0.000000;0.000000}"
                    : $"output at cursor: Master ↔ Source {renderedRepair.DonorSource} crossfade";
            else
                outputAtCursor=$"output at cursor: Source {renderedRepair.DonorSource} inserted (legacy log)";
        }
        else if(!_mapData.HasRepairLog && cc=='A')
            outputAtCursor=$"output at cursor: Source {decisionSource} (legacy decision map)";
        else
            outputAtCursor=$"output at cursor: Master (Source {master}) — no donor inserted";
        string overlay=_repairOverlayEnabled
            ? " • overlay: red replaced Master, light-green crossfade, dark-green donor core, teal supporters, amber Master kept"
            : "";
        string exactTrace=repair is { HasExactCoreTrace:true }
            ? " • donor-coloured Result guide = logged core before final RPM resampling"
            : "";
        string levels=(_levelMatchEnabled
            ? " • donor rows are level-matched for display only"
            : " • donor rows show raw source levels")+exactTrace;
        string reason=_mapData.DecisionReasonAt(_waveformCenterSec);
        string supporters=string.Join(" + ",_mapData.SupportingSourcesAt(_waveformCenterSec));
        string review=cc=='B'
            ? $" • review: {reason}"+(supporters.Length>0?$" • clean witnesses {supporters}":"")
            : reverted ? " • USER UNDO: preview/save uses exact Master" : "";
        WaveformPositionText.Text=$"Master: Source {master} ({masterName}) • Center {_waveformCenterSec:0.000000} s • analysis class {cc} • red vertical = selected time";
        WaveformOutputText.Text=$"{outputAtCursor}{review}{levels}{overlay}";
        ToolTipService.SetToolTip(WaveformOutputText,WaveformOutputText.Text);
        WaveformOutputText.Foreground=new Microsoft.UI.Xaml.Media.SolidColorBrush(
            reverted
                ? Windows.UI.Color.FromArgb(255,95,80,180)
                : repair is not null
                ? Windows.UI.Color.FromArgb(255,15,125,55)
                : cc=='B'
                ? Windows.UI.Color.FromArgb(255,170,105,0)
                : Windows.UI.Color.FromArgb(255,65,90,120));
        WaveformPlot.DrawSplit(MasterWaveformCanvas,DonorWaveformCanvas,
            ResultWaveformCanvas,_sources.Select(s=>s.Path).ToList(),
            _sources.Select(s=>s.Name).ToList(),_lastOutput,
            _mapData,_waveformCenterSec,WaveformSpanSeconds(),_outputSpeedFactor,
            _repairOverlayEnabled,_levelMatchEnabled,_sourceRoles,_revertedRepairs);
        UpdateDonorScrollUi();
        DispatcherQueue.TryEnqueue(UpdateDonorScrollUi);
        UpdateWaveformViewportIndicator(keepVisible:true);
        UpdateCorrectionControls();
    }

    private void DonorWaveformScroll_ViewChanged(object sender,ScrollViewerViewChangedEventArgs e)
        => UpdateDonorScrollUi();

    private void DonorUp_Click(object sender,RoutedEventArgs e)
    {
        double target=Math.Max(0.0,DonorWaveformScroll.VerticalOffset-WaveformPlot.DonorLaneHeight);
        DonorWaveformScroll.ChangeView(null,target,null,true);
    }

    private void DonorDown_Click(object sender,RoutedEventArgs e)
    {
        double target=Math.Min(DonorWaveformScroll.ScrollableHeight,
            DonorWaveformScroll.VerticalOffset+WaveformPlot.DonorLaneHeight);
        DonorWaveformScroll.ChangeView(null,target,null,true);
    }

    private void UpdateDonorScrollUi()
    {
        if(DonorWaveformScroll is null || DonorRangeText is null ||
           DonorUpButton is null || DonorDownButton is null) return;

        int donorCount=_mapData is null ? 0 : Math.Max(0,Math.Min(6,_sources.Count)-1);
        double offset=DonorWaveformScroll.VerticalOffset;
        double viewport=DonorWaveformScroll.ViewportHeight;
        double scrollable=DonorWaveformScroll.ScrollableHeight;
        DonorUpButton.IsEnabled=donorCount>0 && offset>0.5;
        DonorDownButton.IsEnabled=donorCount>0 && offset<scrollable-0.5;

        if(donorCount==0)
        {
            DonorRangeText.Text="—";
            return;
        }

        int visible=Math.Max(1,(int)Math.Ceiling(Math.Max(1.0,viewport)/WaveformPlot.DonorLaneHeight));
        int first=Math.Clamp((int)Math.Floor((offset+0.5)/WaveformPlot.DonorLaneHeight)+1,1,donorCount);
        int last=Math.Min(donorCount,first+visible-1);
        DonorRangeText.Text=$"{first}–{last}\nav {donorCount}";
    }

    private void KeepFileSpeed_Changed(object sender, RoutedEventArgs e)
    {
        // Checked can fire before the following XAML controls are constructed.
        if(RecordedRpm==null || DiscRpm==null || KeepFileSpeed==null)return;
        bool enabled=KeepFileSpeed.IsEnabled && KeepFileSpeed.IsChecked!=true;
        RecordedRpm.IsEnabled=enabled;
        DiscRpm.IsEnabled=enabled;
    }

    private static double SelectedRpmValue(ComboBox box)
    {
        string text=(box.SelectedItem as ComboBoxItem)?.Content?.ToString() ?? "";
        return text switch
        {
            "33⅓" => 100.0/3.0,
            "45" => 45.0,
            "78" => 78.0,
            "77.92 — Europe" => 77.92,
            "78.26 — USA" => 78.26,
            "78.00 — Unknown standard" => 78.0,
            _ => 0.0
        };
    }
}
