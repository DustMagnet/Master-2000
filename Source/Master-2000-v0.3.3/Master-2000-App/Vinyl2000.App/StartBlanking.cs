using Microsoft.UI.Xaml;
using Windows.Storage.Pickers;
namespace Vinyl2000.App;
public sealed partial class MainWindow
{
    private string? _startBlankPreviewPath;
    private string RequireBlankSource()
    {
        if(_engineProcess is not null || _lastOutput is null || !File.Exists(_lastOutput))
            throw new InvalidOperationException("Finish or load a result first.");
        return _lastOutput;
    }
    private static double DetectBlankOnset(string path)
    {
        using var reader=new WaveformPlot.Reader(path);
        var audio=reader.ReadMono(0,reader.SampleRate*5);
        int hop=Math.Max(1,reader.SampleRate/200);
        var levels=new List<double>();
        for(int i=0;i+hop<=audio.Length;++i)
        {
            double sum=0;bool valid=true;
            for(int j=0;j<hop;++j){double v=audio[i+j];if(!double.IsFinite(v)){valid=false;break;}sum+=v*v;}
            if(!valid)break;
            levels.Add(Math.Sqrt(sum/hop));i+=hop-1;
        }
        if(levels.Count<168)throw new InvalidDataException("Not enough leading audio to suggest a boundary.");
        var noise=levels.Take(160).OrderBy(x=>x).ToArray();
        double baseline=(noise[79]+noise[80])/2;
        for(int i=0;i+8<=levels.Count;++i)
        {
            if(levels.Skip(i).Take(8).Count(x=>x>Math.Max(0.008,baseline*5))<7)continue;
            while(i>0 && levels[i-1]>Math.Max(0.003,baseline*2))--i;
            return i*hop/(double)reader.SampleRate;
        }
        throw new InvalidDataException("No clear sustained start found. Set the boundary manually.");
    }
    private void SuggestStartBlank_Click(object sender,RoutedEventArgs e)
    {
        try
        {
            RequireBlankSource();
            if(_mapData is null || _sources.Count<2)throw new InvalidOperationException("Load matching sources and their decision map, or set the boundary manually.");
            var starts=new List<double>();
            foreach(var source in _sources)
            {
                try
                {
                    double onset=DetectBlankOnset(source.Path);
                    using var reader=new WaveformPlot.Reader(source.Path);
                    double aligned=onset-_mapData.DelayAt(source.Number-1,onset)/reader.SampleRate;
                    starts.Add(aligned);
                }
                catch(InvalidDataException) { }
            }
            starts.Sort();double earliest=double.NaN;
            for(int i=0;i+1<starts.Count;++i)
                if(starts[i+1]-starts[i]<=0.020){earliest=starts[i];break;}
            if(!double.IsFinite(earliest) || earliest<0.050)throw new InvalidDataException("Two source starts did not agree within 20 ms. Set and preview manually.");
            StartBlankBoundary.Value=(earliest-0.030)/Math.Max(0.000001,_outputSpeedFactor);
            StartBlankStatus.Text="Two-source onset proposal with 30 ms safety margin. Preview and adjust before saving; weak pickups may start earlier.";
        }
        catch(Exception ex){StartBlankStatus.Text=ex.Message;}
    }
    private void ListenStartOriginal_Click(object sender,RoutedEventArgs e)
    {
        try {string source=RequireBlankSource();_waveformCenterSec=0;PlayRenderedAtCursor(source,"Original result start");}
        catch(Exception ex){StartBlankStatus.Text=ex.Message;}
    }
    private void PreviewStartBlank_Click(object sender,RoutedEventArgs e)
    {
        try
        {
            string source=RequireBlankSource();StopListening();
            string preview=Path.Combine(Path.GetTempPath(),"Master2000-start-"+Guid.NewGuid().ToString("N")+".wav");
            UserCorrection.SaveStartBlank(source,preview,StartBlankBoundary.Value);
            _waveformCenterSec=0;PlayRenderedAtCursor(preview,"Blanked start preview");
            _startBlankPreviewPath=preview;
            StartBlankStatus.Text="Preview only. Original result unchanged. Full level from "+StartBlankBoundary.Value.ToString("0.000")+" s.";
        }
        catch(Exception ex){StartBlankStatus.Text=ex.Message;}
    }
    private async void SaveStartBlank_Click(object sender,RoutedEventArgs e)
    {
        try
        {
            string source=RequireBlankSource();double boundary=StartBlankBoundary.Value;
            var picker=new FileSavePicker();WinRT.Interop.InitializeWithWindow.Initialize(picker,Hwnd);
            picker.SuggestedFileName=Path.GetFileNameWithoutExtension(source)+"-start-blanked";
            picker.FileTypeChoices.Add("PCM WAV",new List<string>{".wav"});
            var target=await picker.PickSaveFileAsync();if(target is null)return;
            if(string.Equals(Path.GetFullPath(target.Path),Path.GetFullPath(UserCorrection.MasterOnlyPath(source)),StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("Choose a new filename; Master-only is protected.");
            if(_sources.Any(item=>string.Equals(Path.GetFullPath(item.Path),Path.GetFullPath(target.Path),StringComparison.OrdinalIgnoreCase) ||
                string.Equals(Path.GetFullPath(item.OriginalPath),Path.GetFullPath(target.Path),StringComparison.OrdinalIgnoreCase)))
                throw new InvalidOperationException("Choose a new filename; input sources are protected.");
            await Task.Run(()=>UserCorrection.SaveStartBlank(source,target.Path,boundary));
            StartBlankStatus.Text="Separate WAV and start-blank audit saved. Engine repair logs describe the original result.";
        }
        catch(Exception ex){StartBlankStatus.Text=ex.Message;}
    }
}
