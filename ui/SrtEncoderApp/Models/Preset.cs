namespace SrtEncoderApp.Models;

/// <summary>
/// Persisted as JSON at %LOCALAPPDATA%\SrtEncoder\Presets\&lt;Id&gt;.json (see
/// Services/PresetService.cs). SchemaVersion exists so a future field rename/removal can
/// migrate old files on load instead of breaking them.
/// </summary>
public sealed class Preset
{
    public int SchemaVersion { get; set; } = 1;
    public string Id { get; set; } = Guid.NewGuid().ToString();
    public string Name { get; set; } = "New Preset";
    public DateTimeOffset CreatedUtc { get; set; } = DateTimeOffset.UtcNow;
    public DateTimeOffset ModifiedUtc { get; set; } = DateTimeOffset.UtcNow;

    public PresetSource Source { get; set; } = new();
    public PresetEncode Encode { get; set; } = new();
    public PresetSrt Srt { get; set; } = new();
}

public sealed class PresetSource
{
    public string Backend { get; set; } = "UVC";
    public string DeviceId { get; set; } = "";
    public string DeviceName { get; set; } = "";
}

public sealed class PresetEncode
{
    public string EncoderImpl { get; set; } = "libx264";
    public string RateControl { get; set; } = "cbr";
    public int BitrateKbps { get; set; } = 6000;
    public int MaxBitrateKbps { get; set; } = 6000;
    public int BufferSizeKbps { get; set; } = 12000;
    public int KeyframeIntervalSec { get; set; } = 2;
    public string X264Preset { get; set; } = "veryfast";
    public int OutputWidth { get; set; } = 1920;
    public int OutputHeight { get; set; } = 1080;
    public int OutputFrameRateNumerator { get; set; } = 30;
    public int OutputFrameRateDenominator { get; set; } = 1;
}

public sealed class PresetSrt
{
    public string Mode { get; set; } = "caller";
    public string Host { get; set; } = "";
    public int Port { get; set; } = 9000;
    public int LatencyMs { get; set; } = 200;

    /// <summary>DPAPI-protected (CurrentUser scope), base64-encoded. Never plaintext at rest — see PresetService.</summary>
    public string? PassphraseProtected { get; set; }

    public int PbKeyLen { get; set; } = 16;
    public string StreamId { get; set; } = "";
}
