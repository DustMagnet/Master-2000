using System;
using System.IO;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Input;
using NAudio.Wave;
using NAudio.Wave.SampleProviders;
namespace Vinyl2000.App;
public sealed partial class MainWindow
{
    private WaveOutEvent? _audioOutput;
    private WaveFileReader? _audioReader;
    private void InitializeListening()
    {
        Content.AddHandler(UIElement.PointerPressedEvent,new PointerEventHandler((s,e)=>StopListening()),true);
        Content.AddHandler(UIElement.PointerWheelChangedEvent,new PointerEventHandler((s,e)=>StopListening()),true);
        Content.AddHandler(UIElement.KeyDownEvent,new KeyEventHandler((s,e)=>StopListening()),true);
        Closed+=(s,e)=>StopListening();
    }
    private void StopListening()
    {
        var output=_audioOutput;_audioOutput=null;
        if(output is not null){output.Stop();output.Dispose();}
        _audioReader?.Dispose();_audioReader=null;
        if(_startBlankPreviewPath is { } preview)
        {
            _startBlankPreviewPath=null;
            try { File.Delete(preview);File.Delete(Path.ChangeExtension(preview,".start-blank.txt")); }
            catch(IOException) { }
            catch(UnauthorizedAccessException) { }
        }
        if(ListenStatus is not null)ListenStatus.Text="Stopped";
    }
    private void StopAudio_Click(object sender,RoutedEventArgs e)=>StopListening();
    private void PlayWhole_Click(object sender,RoutedEventArgs e)=>PlaySource(0,double.PositiveInfinity,false);
    private void PlayRange_Click(object sender,RoutedEventArgs e)=>PlaySource(ListenStart.Value,ListenEnd.Value,false);
    private void PlayCursor_Click(object sender,RoutedEventArgs e)=>PlaySource(0,0,true);
    private void PlayResultCursor_Click(object sender,RoutedEventArgs e)
    {
        if(_lastOutput is not null) PlayRenderedAtCursor(_lastOutput,"Result");
    }
    private void PlayMasterCursor_Click(object sender,RoutedEventArgs e)
    {
        if(_lastOutput is not null)
            PlayRenderedAtCursor(UserCorrection.MasterOnlyPath(_lastOutput),"Master-only");
    }

    private void PlayRenderedAtCursor(string path,string label)
    {
        StopListening();
        if(_engineProcess is not null){ListenStatus.Text="Wait for processing to finish.";return;}
        try
        {
            if(!File.Exists(path)) throw new FileNotFoundException("Audio file not found.",path);
            var reader=new WaveFileReader(path);_audioReader=reader;
            double duration=reader.TotalTime.TotalSeconds;
            double center=_waveformCenterSec/Math.Max(0.000001,_outputSpeedFactor);
            double start=Math.Max(0,center-5);
            double end=Math.Min(duration,start+10);
            start=Math.Max(0,end-10);
            reader.CurrentTime=TimeSpan.FromSeconds(start);
            var segment=new OffsetSampleProvider(reader.ToSampleProvider())
                {Take=TimeSpan.FromSeconds(Math.Max(0.001,end-start))};
            var output=new WaveOutEvent();_audioOutput=output;
            output.PlaybackStopped+=(s,e)=>DispatcherQueue.TryEnqueue(()=>
            {
                if(ReferenceEquals(_audioOutput,output))
                {
                    StopListening();
                    if(e.Exception is not null) ListenStatus.Text=e.Exception.Message;
                }
            });
            output.Init(segment);output.Play();
            ListenStatus.Text=$"{label}: {start:0.00}–{end:0.00} s";
        }
        catch(Exception ex){StopListening();ListenStatus.Text="Playback: "+ex.Message;}
    }
    private void PlaySource(double start,double end,bool aroundCursor)
    {
        StopListening();
        if(_engineProcess is not null){ListenStatus.Text="Wait for processing to finish.";return;}
        if(SourcesList.SelectedItem is not SourceItem source){ListenStatus.Text="Select a source first.";return;}
        try {
            var reader=new WaveFileReader(source.Path);_audioReader=reader;
            double duration=reader.TotalTime.TotalSeconds;
            if(aroundCursor){double center=_waveformCenterSec;
                if(_mapData is not null)center+=_mapData.DelayAt(source.Number-1,center)/reader.WaveFormat.SampleRate;
                start=Math.Max(0,center-5);end=Math.Min(duration,start+10);start=Math.Max(0,end-10);
            }
            if(!double.IsFinite(start)||double.IsNaN(end)||start<0||end<=start||start>=duration)throw new InvalidDataException("Choose a valid start/end inside the source file.");
            end=Math.Min(end,duration);
            reader.CurrentTime=TimeSpan.FromSeconds(start);
            var segment=new OffsetSampleProvider(reader.ToSampleProvider()) {Take=TimeSpan.FromSeconds(end-start)};
            var output=new WaveOutEvent();_audioOutput=output;
            output.PlaybackStopped+=(s,e)=>DispatcherQueue.TryEnqueue(()=>{if(ReferenceEquals(_audioOutput,output)){StopListening();if(e.Exception is not null)ListenStatus.Text=e.Exception.Message;}});
            output.Init(segment);output.Play();
            ListenStatus.Text=$"{source.Name}: {start:0.00}–{end:0.00} s • original channels / recorded speed";
        } catch(Exception ex){StopListening();ListenStatus.Text="Playback: "+ex.Message;}
    }
}
