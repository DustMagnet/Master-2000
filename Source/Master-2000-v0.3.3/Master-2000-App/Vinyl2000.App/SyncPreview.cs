using System;
using System.IO;
using Path = System.IO.Path;
using System.Linq;
using System.Diagnostics;
using System.Globalization;
using System.Threading.Tasks;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;
namespace Vinyl2000.App;

public sealed partial class MainWindow
{
    private sealed record SyncChoice(int Master,double[] Offsets,bool WasAutomatic);
    private sealed record SourceAssessment(
        int Source,bool Recommended,bool Selected,string RecommendedRole,
        string SelectedRole,double Quality,double ProgrammeLevelDbfs,
        double HeadroomDb,double CleanPercent,long ActiveWindows,
        long ImpulseWindows,double ImpulsesPerMinute,long ClippedSamples,
        double SyncSupportPercent,double SecondSupportPercent,int ViableDonors,
        int ClusterSize,int StrongDegree,double DetailPenalty);

    private static List<SourceAssessment> LoadSourceAssessments(string path)
    {
        var result=new List<SourceAssessment>();
        if(!File.Exists(path)) return result;
        foreach(string line in File.ReadLines(path).Skip(1))
        {
            string[] p=line.Split(',');
            if(p.Length<19) continue;
            int I(int index)=>int.Parse(p[index],CultureInfo.InvariantCulture);
            long L(int index)=>long.Parse(p[index],CultureInfo.InvariantCulture);
            double D(int index)=>double.Parse(p[index],CultureInfo.InvariantCulture);
            result.Add(new SourceAssessment(
                I(0),p[1]=="1",p[2]=="1",p[3],p[4],D(5),D(6),D(7),D(8),
                L(9),L(10),D(11),L(12),D(13),D(14),I(15),I(16),I(17),D(18)));
        }
        return result.OrderBy(row=>row.Source).ToList();
    }

    private async Task<SyncChoice?> PrepareSyncPreviewAsync()
    {
        StopListening();
        _analysisWatch.Restart();
        StartAnalysisHeartbeat("Start-sync preflight");
        string temp=Path.Combine(Path.GetTempPath(),"Master2000-sync-"+Guid.NewGuid().ToString("N")+".csv");
        string[] paths=_sources.Select(s=>s.Path).ToArray();
        string[] names=_sources.Select(s=>s.Name).ToArray();
        bool wasAutomatic=MasterMode.SelectedIndex==0;
        var assessments=new List<SourceAssessment>();
        try {
            SetBusy(true);StatusText.Text="Checking start sync across several music sections...";
            AnalysisProgress.IsIndeterminate=false;AnalysisProgress.Minimum=0;
            AnalysisProgress.Maximum=100;AnalysisProgress.Value=0;ProgressText.Text="0%";
            var automatic=new double[paths.Length];var verified=new bool[paths.Length];
            var statuses=new string[paths.Length];int master=1;
            async Task RunPreview(double[]? hints=null,int? forcedMaster=null) {
                var psi=new ProcessStartInfo(Path.Combine(AppContext.BaseDirectory,"Master2000Engine.exe")) {
                    UseShellExecute=false,CreateNoWindow=true,RedirectStandardError=true,RedirectStandardOutput=true
                };
                foreach(string arg in new[]{paths.Length.ToString(),temp+".unused.wav","--master",
                    hints==null
                        ? (forcedMaster.HasValue
                            ? forcedMaster.Value.ToString(CultureInfo.InvariantCulture)
                            : (MasterMode.SelectedIndex==0?"auto":MasterMode.SelectedIndex.ToString()))
                        : master.ToString(CultureInfo.InvariantCulture),
                    "--channel-mode",ChannelModeToggle.IsOn?"stereo":"mono","--preview-sync",temp})psi.ArgumentList.Add(arg);
                if(hints!=null) {
                    psi.ArgumentList.Add("--sync-offsets");
                    psi.ArgumentList.Add(string.Join(",",hints.Select(v=>v.ToString("R",CultureInfo.InvariantCulture))));
                }
                foreach(string path in paths)psi.ArgumentList.Add(path);
                using var process=new Process {StartInfo=psi,EnableRaisingEvents=true};
                var errors=new System.Text.StringBuilder();
                process.ErrorDataReceived+=(_,args)=> {
                    if(string.IsNullOrWhiteSpace(args.Data)) return;
                    string line=args.Data.Trim();
                    lock(errors) errors.AppendLine(line);
                    if(line.StartsWith("PREFLIGHT_PROGRESS ",StringComparison.Ordinal)) {
                        string[] q=line.Split(' ',StringSplitOptions.RemoveEmptyEntries);
                        if(q.Length>=5 && int.TryParse(q[1],out int done) &&
                           int.TryParse(q[2],out int total) &&
                           int.TryParse(q[3],out int candidate) &&
                           int.TryParse(q[4],out int donor))
                            DispatcherQueue.TryEnqueue(()=> {
                                double percent=total>0?100.0*done/total:0.0;
                                AnalysisProgress.Value=Math.Clamp(percent,0,100);
                                ProgressText.Text=$"Start-sync preflight • phase {percent:0.0}%";
                                StatusText.Text="Checking every source pair...";
                                PositionText.Text=$"Preflight {done}/{total} • Source {candidate} ↔ Source {donor}";
                                SetAnalysisHeartbeatPhase($"Start-sync preflight {done}/{total}");
                            });
                    } else if(line.StartsWith("PHASE_MAP ",StringComparison.Ordinal)) {
                        string[] q=line.Split(' ',StringSplitOptions.RemoveEmptyEntries);
                        if(q.Length>=4 && int.TryParse(q[1],out int ordinal) &&
                           int.TryParse(q[2],out int total) && int.TryParse(q[3],out int percent))
                            DispatcherQueue.TryEnqueue(()=> {
                                AnalysisProgress.IsIndeterminate=false;
                                AnalysisProgress.Value=Math.Clamp(percent,0,100);
                                ProgressText.Text=$"Start-sync map {ordinal}/{total} • phase {percent}%";
                                StatusText.Text="Verifying start sync...";
                                PositionText.Text=$"Tidskarta {ordinal}/{total} — {percent}%";
                                SetAnalysisHeartbeatPhase($"Start-sync tidskarta {ordinal}/{total} — {percent}%");
                            });
                    }
                    else if(line.StartsWith("PHASE ",StringComparison.Ordinal)) {
                        var fields=line.Split(' ',StringSplitOptions.RemoveEmptyEntries);
                        if(fields.Length>1 && fields[1] is "GLOBAL_ALIGNMENT" or "TIME_MAP")
                            DispatcherQueue.TryEnqueue(()=> {
                                AnalysisProgress.IsIndeterminate=true;AnalysisProgress.Value=0;
                                ProgressText.Text=fields[1]=="TIME_MAP"?"Start-sync maps • working":"Start alignment • working";
                                SetAnalysisHeartbeatPhase(ProgressText.Text);
                            });
                    }
                };
                _engineProcess=process;process.Start();process.BeginErrorReadLine();
                var output=process.StandardOutput.ReadToEndAsync();
                await process.WaitForExitAsync();process.WaitForExit();await output;
                AnalysisProgress.IsIndeterminate=false;AnalysisProgress.Value=0;
                ProgressText.Text="Start-sync check finished • preparing source review";
                string error;lock(errors) error=errors.ToString();
                _engineProcess=null;
                if(process.ExitCode!=0) {
                    string? concise=error.Split(new[]{'\r','\n'},StringSplitOptions.RemoveEmptyEntries)
                        .LastOrDefault(line=>line.TrimStart().StartsWith("ERROR:",StringComparison.Ordinal));
                    throw new InvalidOperationException(concise is null
                        ? error : concise[(concise.IndexOf("ERROR:",StringComparison.Ordinal)+"ERROR:".Length)..].Trim());
                }
                var seen=new bool[paths.Length];
                foreach(var line in File.ReadLines(temp).Skip(1)) {
                    var fields=line.Split(',');int i=int.Parse(fields[0],CultureInfo.InvariantCulture)-1;
                    if(i<0||i>=paths.Length||fields.Length<6)throw new InvalidOperationException("Invalid sync preview data");
                    master=int.Parse(fields[1],CultureInfo.InvariantCulture);
                    automatic[i]=double.Parse(fields[2],CultureInfo.InvariantCulture);
                    verified[i]=fields[3]=="1";statuses[i]=fields[5];seen[i]=true;
                }
                if(seen.Any(v=>!v))throw new InvalidOperationException("Incomplete sync preview data");
                assessments=LoadSourceAssessments(temp+".assessment.csv");
            }
            await RunPreview();
            _analysisWatch.Stop();StopAnalysisHeartbeat();CancelButton.IsEnabled=false;

            if(assessments.Count==paths.Length)
            {
                int chosenMaster=master;
                var assessmentPanel=new StackPanel {Spacing=10,MinWidth=760};
                assessmentPanel.Children.Add(new TextBlock {
                    Text="Source assessment is completed before restoration. Programme level is shown only to explain amplitude differences; louder does not mean cleaner. Master-2000 recommends a source only when it also has at least two independently syncable donors.",
                    TextWrapping=TextWrapping.Wrap
                });
                assessmentPanel.Children.Add(new TextBlock {
                    Text="Preflight clean windows = active 20 ms windows without a large impulse/crest outlier. Preflight sync support = mean support from the two best donors. These are comparative measurements, not a promise that a source is fault-free.",
                    TextWrapping=TextWrapping.Wrap,Opacity=0.68,FontSize=12
                });
                var choices=new StackPanel {Spacing=6};
                foreach(var row in assessments)
                {
                    int qualityRank=1+assessments.Count(other=>other.Quality>row.Quality);
                    string recommendation=row.Recommended?"  • RECOMMENDED":"";
                    string unavailable=row.ViableDonors<2?"  • fewer than 2 viable donors":"";
                    var radio=new RadioButton {
                        GroupName="SourceAssessmentMaster",
                        IsChecked=row.Source==master,
                        IsEnabled=row.ViableDonors>=2,
                        Content=new TextBlock {
                            Text=$"Source {row.Source}: {names[row.Source-1]}{recommendation}{unavailable}\n"+
                                 $"Quality rank {qualityRank}/{assessments.Count} (score {row.Quality:0.00})  • preflight clean windows {row.CleanPercent:0.0}%  • coarse impulse windows {row.ImpulseWindows} ({row.ImpulsesPerMinute:0.0}/min)  • preflight sync support {row.SyncSupportPercent:0.0}%\n"+
                                 $"Typical level {row.ProgrammeLevelDbfs:0.0} dBFS  • headroom {row.HeadroomDb:0.0} dB  • clipped samples {row.ClippedSamples}  • viable donors {row.ViableDonors}  • proposed role {row.RecommendedRole}",
                            TextWrapping=TextWrapping.Wrap,FontSize=12
                        }
                    };
                    int sourceNumber=row.Source;
                    radio.Checked+=(_,__)=>chosenMaster=sourceNumber;
                    choices.Children.Add(new Border {
                        BorderBrush=new SolidColorBrush(row.Recommended
                            ? Color.FromArgb(255,25,135,75)
                            : Color.FromArgb(255,205,210,216)),
                        BorderThickness=new Thickness(row.Recommended?2:1),
                        CornerRadius=new CornerRadius(6),Padding=new Thickness(8),
                        Child=radio
                    });
                }
                assessmentPanel.Children.Add(choices);
                var assessmentDialog=new ContentDialog {
                    XamlRoot=Content.XamlRoot,
                    Title="Source assessment — choose Master",
                    PrimaryButtonText="Use selected Master",
                    CloseButtonText="Cancel",
                    Content=new ScrollViewer {MaxHeight=590,Content=assessmentPanel}
                };
                assessmentDialog.Resources["ContentDialogMaxWidth"]=900.0;
                if(await assessmentDialog.ShowAsync()!=ContentDialogResult.Primary)
                    return null;
                if(chosenMaster!=master)
                {
                    wasAutomatic=false;
                    MasterMode.SelectedIndex=chosenMaster;
                    _analysisWatch.Restart();StartAnalysisHeartbeat("Rechecking selected Master");
                    StatusText.Text=$"Rechecking Source {chosenMaster} as Master...";
                    try { await RunPreview(null,chosenMaster); }
                    finally {_analysisWatch.Stop();StopAnalysisHeartbeat();}
                }
            }
            bool allVerified=verified.Select((value,index)=>index==master-1 || value).All(value=>value);
            if(allVerified)
            {
                StatusText.Text="Alla källor verifierade";
                PositionText.Text="Start sync verified automatically — no manual adjustment needed.";
                ResultInfo.Severity=InfoBarSeverity.Success;
                ResultInfo.Title="Alla källor verifierade";
                ResultInfo.Message="Försynken ligger rätt. Master-2000 fortsätter automatiskt till filvalet.";
                ResultInfo.IsOpen=true;
                return new SyncChoice(master,(double[])automatic.Clone(),wasAutomatic);
            }
            var offsets=(double[])automatic.Clone();var changed=new bool[paths.Length];
            double? anchor=null;
            var panel=new StackPanel {Spacing=10,MinWidth=680};
            var verdict=new TextBlock {TextWrapping=TextWrapping.Wrap,FontWeight=Microsoft.UI.Text.FontWeights.SemiBold};
            string SourceVerdict(bool isVerified,int i) {
                if(i==master-1)return "MASTER";
                if(statuses[i]=="PROGRAMME_PATH_VERIFIED_ONSET_CONFLICT")
                    return "music path verified; first edge differs — blocked at start";
                return isVerified?"start sync verified":"unverified — adjust and check";
            }
            void ShowVerdict()=>verdict.Text=string.Join(" • ",verified.Select((v,i)=>
                $"Source {i+1}: {SourceVerdict(v,i)}"));
            ShowVerdict();panel.Children.Add(verdict);
            panel.Children.Add(new TextBlock {Text="Click a music detail in the MASTER row, then the same detail in another row. Or adjust its offset below. After manual adjustments, press Verify start sync. Full restoration still verifies the local alignment throughout the recording.",TextWrapping=TextWrapping.Wrap});
            var navigation=new StackPanel {Orientation=Orientation.Horizontal,Spacing=10};
            var center=new NumberBox {Header="Center (seconds)",Value=1,Minimum=0,Maximum=_sources.Max(s=>s.Format?.Seconds??0),SmallChange=.1,Width=180,SpinButtonPlacementMode=NumberBoxSpinButtonPlacementMode.Compact};
            var zoom=new ComboBox {Header="Window",Width=140,SelectedIndex=0};
            foreach(string label in new[]{"2 seconds","500 ms","100 ms","20 ms"})zoom.Items.Add(label);
            navigation.Children.Add(center);navigation.Children.Add(zoom);panel.Children.Add(navigation);
            var canvas=new Canvas {Width=680,Height=paths.Length*85,Background=new SolidColorBrush(Color.FromArgb(255,248,250,252))};panel.Children.Add(canvas);
            var info=new TextBlock {Text="Select a point on the master first.",TextWrapping=TextWrapping.Wrap};panel.Children.Add(info);
            var edits=new NumberBox[paths.Length];bool updating=false;
            double span()=>new[]{2.0,.5,.1,.02}[Math.Clamp(zoom.SelectedIndex,0,3)];
            void Draw() {
                if(!double.IsFinite(center.Value))return;
                canvas.Children.Clear();double start=center.Value-span()/2;
                for(int lane=0;lane<paths.Length;++lane) {
                    using var reader=new WaveformPlot.Reader(paths[lane]);
                    int count=Math.Max(2,(int)(span()*reader.SampleRate));
                    var samples=reader.ReadMono((long)Math.Round((start+offsets[lane])*reader.SampleRate),count);
                    double peak=Math.Max(.0001,samples.Where(float.IsFinite).Select(x=>Math.Abs((double)x)).DefaultIfEmpty(0).Max());
                    var label=new TextBlock {Text=$"{lane+1}: {names[lane]}"+(lane==master-1?"  MASTER":""),FontSize=11};Canvas.SetTop(label,lane*85);canvas.Children.Add(label);
                    var curve=new Polyline {Stroke=new SolidColorBrush(Color.FromArgb(255,0,103,192)),StrokeThickness=1};
                    for(int x=0;x<660;++x){float v=samples[Math.Min(count-1,x*count/660)];if(float.IsFinite(v))curve.Points.Add(new Point(10+x,lane*85+49-v/peak*24));}
                    canvas.Children.Add(curve);
                }
                if(anchor.HasValue) {
                    double x=10+(anchor.Value-start)/span()*660;
                    if(x>=10 && x<=670)canvas.Children.Add(new Line {X1=x,X2=x,Y1=0,Y2=canvas.Height,Stroke=new SolidColorBrush(Color.FromArgb(255,210,65,0)),StrokeThickness=1});
                }
            }
            for(int i=0;i<paths.Length;++i) {
                int lane=i;var box=new NumberBox {Header=$"Source {i+1} offset (ms)"+(i==master-1?" — master":""),Value=offsets[i]*1000,SmallChange=1,LargeChange=10,IsEnabled=i!=master-1,SpinButtonPlacementMode=NumberBoxSpinButtonPlacementMode.Compact};
                edits[i]=box;panel.Children.Add(box);
                box.ValueChanged+=(s,e)=>{if(updating||!double.IsFinite(box.Value))return;offsets[lane]=box.Value/1000;changed[lane]=true;verified[lane]=false;statuses[lane]="MANUAL_HINT_UNVERIFIED";ShowVerdict();Draw();};
            }
            canvas.PointerPressed+=(s,e)=> {
                var pt=e.GetCurrentPoint(canvas).Position;int lane=(int)(pt.Y/85);
                if(lane<0||lane>=paths.Length||pt.X<10||pt.X>670||!double.IsFinite(center.Value))return;
                double time=center.Value-span()/2+(pt.X-10)/660*span();
                if(lane==master-1){anchor=time;info.Text="Master point selected. Click the corresponding detail in a source row.";}
                else if(anchor.HasValue){offsets[lane]+=time-anchor.Value;changed[lane]=true;verified[lane]=false;statuses[lane]="MANUAL_HINT_UNVERIFIED";ShowVerdict();updating=true;edits[lane].Value=offsets[lane]*1000;updating=false;info.Text=$"Source {lane+1} adjusted. This is a search hint, not a verified repair.";}
                Draw();
            };
            var savedVerified=(bool[])verified.Clone();var savedStatuses=(string[])statuses.Clone();
            var reset=new Button {Content="Reset to automatic"};reset.Click+=(s,e)=>{updating=true;for(int i=0;i<paths.Length;++i){offsets[i]=automatic[i];changed[i]=false;verified[i]=savedVerified[i];statuses[i]=savedStatuses[i];edits[i].Value=offsets[i]*1000;}updating=false;anchor=null;ShowVerdict();Draw();};panel.Children.Add(reset);
            center.ValueChanged+=(s,e)=>Draw();zoom.SelectionChanged+=(s,e)=>Draw();Draw();
            var previewControls=new ContentControl {Content=panel};
            var dialog=new ContentDialog {XamlRoot=Content.XamlRoot,Title="1. Check start sync",PrimaryButtonText="2. Continue restoration",CloseButtonText="Cancel",Content=new ScrollViewer {MaxHeight=560,Content=previewControls}};
            bool verifying=false;
            dialog.Closing+=(s,e)=>{if(verifying)e.Cancel=true;};
            var verify=new Button {Content="Verify start sync"};panel.Children.Add(verify);
            verify.Click+=async (s,e)=> {
                verifying=true;previewControls.IsEnabled=false;dialog.IsPrimaryButtonEnabled=false;dialog.IsSecondaryButtonEnabled=false;
                dialog.CloseButtonText="";info.Text="Checking several music sections...";
                try {
                    _analysisWatch.Restart();StartAnalysisHeartbeat("Verifying adjusted start sync");
                    await RunPreview((double[])offsets.Clone());
                    updating=true;
                    for(int i=0;i<paths.Length;++i) {
                        offsets[i]=automatic[i];savedVerified[i]=verified[i];changed[i]=true;edits[i].Value=offsets[i]*1000;
                        savedStatuses[i]=statuses[i];
                    }
                    updating=false;anchor=null;ShowVerdict();Draw();
                    info.Text="Check finished. Verified means start alignment only; each repair still requires local checks.";
                } catch(Exception ex) {
                    Array.Fill(verified,false);verified[master-1]=true;ShowVerdict();info.Text="Sync check failed: "+ex.Message;
                } finally {
                    _analysisWatch.Stop();StopAnalysisHeartbeat();
                    verifying=false;updating=false;previewControls.IsEnabled=true;dialog.IsPrimaryButtonEnabled=true;dialog.CloseButtonText="Cancel";
                }
            };
            dialog.Resources["ContentDialogMaxWidth"]=800.0;
            var result=await dialog.ShowAsync();
            if(result!=ContentDialogResult.Primary)return null;
            return new SyncChoice(master,offsets.Select((v,i)=>(changed[i]||verified[i])?v:double.NaN).ToArray(),wasAutomatic);
        } catch(Exception ex) {ShowFailure("Sync preview: "+ex.Message);return null;}
        finally {
            _engineProcess=null;_analysisWatch.Stop();StopAnalysisHeartbeat();SetBusy(false);
            if(File.Exists(temp))File.Delete(temp);
            if(File.Exists(temp+".assessment.csv"))File.Delete(temp+".assessment.csv");
        }
    }
}
