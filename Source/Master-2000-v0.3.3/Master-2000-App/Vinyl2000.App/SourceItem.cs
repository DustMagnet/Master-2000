namespace Vinyl2000.App;
public sealed class SourceItem
{
    public int Number { get; set; }
    public string OriginalPath { get; init; } = "";
    public string Path { get; init; } = "";
    public string Name => System.IO.Path.GetFileName(
        string.IsNullOrWhiteSpace(OriginalPath) ? Path : OriginalPath);
    public string Details { get; init; } = "WAV";
    public WavFormat? Format { get; init; }
}
