using System.Diagnostics;
using System.Globalization;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Storage.Pickers;

namespace Vinyl2000.App;

public sealed partial class MainWindow
{
    private async void TrySource_Click(object sender,RoutedEventArgs e)
    {
        if(_engineProcess is not null || _mapData is null || _lastOutput is null ||
           _sources.Count<3 || _sources.Count>6 || _sources.Any(s=>!File.Exists(s.Path))) return;
        string originalResult=_lastOutput;
        string working=Path.Combine(Path.GetTempPath(),"Master2000-review-"+Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(working);
        string? preview=null;
        string? generatedLog=null;
        string beforePath=originalResult;
        var sourceChoice=new ComboBox {Header="Replacement source",HorizontalAlignment=HorizontalAlignment.Stretch};
        foreach(var source in _sources)
            if(source.Number!=_mapData.MasterSource &&
               (!_sourceRoles.TryGetValue(source.Number,out string? role) || role!="REJECTED"))
                sourceChoice.Items.Add(new ComboBoxItem {Content=$"S{source.Number}: {source.Name}",Tag=source.Number});
        if(sourceChoice.Items.Count==0)return;
        sourceChoice.SelectedIndex=0;
        double half=Math.Min(.005,WaveformSpanSeconds()/2);
        var start=new NumberBox {Header="Start (recorded seconds)",Value=Math.Max(0,_waveformCenterSec-half),SmallChange=.0001};
        var end=new NumberBox {Header="End (recorded seconds; at most 20 ms)",Value=Math.Min(_mapData.DurationSec,_waveformCenterSec+half),SmallChange=.0001};
        var status=new TextBlock {Text="Click the waveform to position the cursor, then enter the short interval. The selected source needs an independent clean witness. Only verified impulse cores and their fades are changed.",TextWrapping=TextWrapping.Wrap,MaxWidth=540};
        var tryButton=new Button {Content="Test selected source"};
        var before=new Button {Content="Listen before",IsEnabled=false};
        var after=new Button {Content="Listen preview",IsEnabled=false};
        var stop=new Button {Content="Stop"};
        var discard=new Button {Content="Discard preview",IsEnabled=false};
        var save=new Button {Content="Approve and save new WAV",IsEnabled=false};
        var actions=new StackPanel {Orientation=Orientation.Horizontal,Spacing=8};
        actions.Children.Add(before);actions.Children.Add(after);actions.Children.Add(stop);
        var panel=new StackPanel {Spacing=10,MaxWidth=560};
        panel.Children.Add(sourceChoice);panel.Children.Add(start);panel.Children.Add(end);
        panel.Children.Add(tryButton);panel.Children.Add(status);panel.Children.Add(actions);
        panel.Children.Add(discard);panel.Children.Add(save);
        var dialog=new ContentDialog {XamlRoot=Content.XamlRoot,Title="Try a source for this interval",Content=panel,CloseButtonText="Close"};
        void Invalidate() {preview=null;generatedLog=null;before.IsEnabled=false;after.IsEnabled=false;discard.IsEnabled=false;save.IsEnabled=false;StopListening();}
        start.ValueChanged+=(s,args)=>Invalidate();end.ValueChanged+=(s,args)=>Invalidate();sourceChoice.SelectionChanged+=(s,args)=>Invalidate();
        before.Click+=(s,args)=>PlayRenderedAtCursor(beforePath,"Before local review");
        after.Click+=(s,args)=>{if(preview is not null)PlayRenderedAtCursor(preview,"Local review preview");};
        stop.Click+=(s,args)=>StopListening();
        discard.Click+=(s,args)=>{Invalidate();status.Text="Preview discarded. Original Result retained.";};
        tryButton.Click+=async (s,args)=>
        {
            Invalidate();
            if(!double.IsFinite(start.Value)||!double.IsFinite(end.Value)||start.Value<0||end.Value<=start.Value||end.Value-start.Value>.020001) {status.Text="Choose a valid interval no wider than 20 ms.";return;}
            var selected=sourceChoice.SelectedItem as ComboBoxItem;
            if(selected?.Tag is not int donor)return;
            tryButton.IsEnabled=false;sourceChoice.IsEnabled=false;start.IsEnabled=false;end.IsEnabled=false;
            dialog.IsEnabled=false;
            Process? process=null;
            try
            {
                status.Text="Checking local timing, two independent sources and the rendered seams…";
                string baseline=originalResult;
                if(_revertedRepairs.Count>0)
                {
                    baseline=Path.Combine(working,"before.wav");
                    await Task.Run(()=>UserCorrection.Save(originalResult,UserCorrection.MasterOnlyPath(originalResult),baseline,_revertedRepairs.ToList(),_outputSpeedFactor));
                }
                beforePath=baseline;
                string output=Path.Combine(working,"preview-"+Guid.NewGuid().ToString("N")+".wav");
                var format=WavInfo.Read(originalResult) ?? throw new InvalidDataException("Cannot read Result format.");
                var info=new ProcessStartInfo(Path.Combine(AppContext.BaseDirectory,"Master2000Engine.exe")) {UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true};
                string N(double value)=>value.ToString("R",CultureInfo.InvariantCulture);
                info.ArgumentList.Add(_sources.Count.ToString(CultureInfo.InvariantCulture));info.ArgumentList.Add(output);
                info.ArgumentList.Add("--master");info.ArgumentList.Add(_mapData.MasterSource.ToString(CultureInfo.InvariantCulture));
                info.ArgumentList.Add("--channel-mode");info.ArgumentList.Add(format.Channels==1?"mono":"stereo");
                info.ArgumentList.Add("--local-review");info.ArgumentList.Add(donor.ToString(CultureInfo.InvariantCulture));
                info.ArgumentList.Add(N(start.Value));info.ArgumentList.Add(N(end.Value));info.ArgumentList.Add(N(_outputSpeedFactor));info.ArgumentList.Add(baseline);
                double center=(start.Value+end.Value)/2;
                info.ArgumentList.Add(string.Join(",",_sources.Select(source=>N(_mapData.DelayAt(source.Number-1,center)))));
                info.ArgumentList.Add(string.Join(",",_sources.Select(source=>
                    _sourceRoles.TryGetValue(source.Number,out string? role) && role.Equals("REJECTED",StringComparison.OrdinalIgnoreCase)?"0":"1")));
                foreach(var source in _sources)info.ArgumentList.Add(source.Path);
                process=new Process {StartInfo=info};process.Start();_engineProcess=process;SetBusy(true);
                Task<string> stdout=process.StandardOutput.ReadToEndAsync(),stderr=process.StandardError.ReadToEndAsync();
                await process.WaitForExitAsync();string report=await stdout;string error=await stderr;
                if(process.ExitCode!=0 || !File.Exists(output))throw new InvalidOperationException(error.Trim().Length>0?error.Trim():"Local review was rejected.");
                preview=output;generatedLog=output+".local-review.csv";
                status.Text=report.Trim()+". Preview ready; listen before/after, then approve or discard.";
                before.IsEnabled=after.IsEnabled=discard.IsEnabled=save.IsEnabled=true;
                _waveformCenterSec=center;
            }
            catch(Exception ex){status.Text="Kept original Result: "+ex.Message;}
            finally {if(ReferenceEquals(_engineProcess,process))_engineProcess=null;process?.Dispose();SetBusy(false);dialog.IsEnabled=true;tryButton.IsEnabled=true;sourceChoice.IsEnabled=true;start.IsEnabled=true;end.IsEnabled=true;}
        };
        save.Click+=async (s,args)=>
        {
            if(preview is null || generatedLog is null)return;
            StopListening();
            var picker=new FileSavePicker {SuggestedFileName=Path.GetFileNameWithoutExtension(originalResult)+"-local-review"};
            WinRT.Interop.InitializeWithWindow.Initialize(picker,Hwnd);picker.FileTypeChoices.Add("PCM WAV",new List<string>{".wav"});
            var destination=await picker.PickSaveFileAsync();if(destination is null)return;
            try
            {
                string target=Path.GetFullPath(destination.Path);
                if(string.Equals(target,Path.GetFullPath(originalResult),StringComparison.OrdinalIgnoreCase) ||
                   _sources.Any(source=>string.Equals(target,Path.GetFullPath(source.Path),StringComparison.OrdinalIgnoreCase)) ||
                   string.Equals(target,Path.GetFullPath(UserCorrection.MasterOnlyPath(originalResult)),StringComparison.OrdinalIgnoreCase))
                    throw new InvalidOperationException("Choose a new filename; sources and original Result are protected.");
                File.Copy(preview,target,true);
                File.Copy(generatedLog,Path.ChangeExtension(target,".local-review.csv"),true);
                string undoLog=Path.ChangeExtension(beforePath,".user-corrections.csv");
                if(File.Exists(undoLog))File.Copy(undoLog,Path.ChangeExtension(target,".user-corrections.csv"),true);
                status.Text="Approved WAV and local repair log saved. The original Result is retained.";
            }
            catch(Exception ex){status.Text="Save: "+ex.Message;}
        };
        try {await dialog.ShowAsync();}
        finally {StopListening();try{Directory.Delete(working,true);}catch{}DrawWaveforms();}
    }
}
