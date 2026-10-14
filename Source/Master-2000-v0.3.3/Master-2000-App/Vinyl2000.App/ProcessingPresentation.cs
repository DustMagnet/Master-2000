using Microsoft.UI.Xaml;
namespace Vinyl2000.App;
public sealed partial class MainWindow
{
    private string _workPhaseCode="";
    private static string WorkPhaseLabel(string code)=>code switch {
        "BACKWARD_SYNC"=>"Final start sync",
        "RENDER_REPAIRS"=>"Inserting repairs and checking seams",
        "EDGE_SYNC"=>"Following sync toward track edges",
            "DROPOUT_REVIEW"=>"Checking Master dropouts",
        "LOCAL_REVIEW"=>"Reviewing remaining impulses",
        "FINAL_SUMMARY"=>"Preparing repair summary",
        "MASTER_COMPARISON"=>"Comparing Result with Master",
        "SPEED_RESULT"=>"Preparing Result playback speed",
        "SPEED_MASTER"=>"Preparing Master comparison speed",
        "OUTPUT_QA"=>"Checking output quality",
        "SAVE_RESULT"=>"Saving Result WAV",
        "SAVE_MASTER"=>"Saving Master comparison WAV",
        "FINAL_FILES"=>"Finishing diagnostics",
        "SAVE_MAP"=>"Saving decision map",
        _=>code
    };
    private void ApplyWorkPhase(string code)
    {
        _workPhaseCode=code;
        string label=WorkPhaseLabel(code);
        AnalysisProgress.IsIndeterminate=true;AnalysisProgress.Value=0;
        ProgressText.Text=label+" • working";
        StatusText.Text=label+"...";PositionText.Text="";
        SetAnalysisHeartbeatPhase(label);
    }
    private void ApplyWorkProgress(string code,long done,long total)
    {
        if(_workPhaseCode!=code)ApplyWorkPhase(code);
        double percent=Math.Clamp(100.0*done/Math.Max(1,total),0,100);
        string label=WorkPhaseLabel(code);
        AnalysisProgress.IsIndeterminate=false;AnalysisProgress.Value=percent;
        ProgressText.Text=$"{label} • phase {percent:0.0}%";
        PositionText.Text=$"{done:N0} / {total:N0} work items";
    }
}
