using System.Collections.ObjectModel;
using SrtEncoderApp.Models;
using SrtEncoderApp.Services;

namespace SrtEncoderApp.ViewModels;

/// <summary>
/// Holds all editable state for the single-page Phase 1 UI (device selection, encode
/// settings, SRT settings, presets, live stats) and mediates between the view and
/// CaptureCoreService/PresetService. SRT passphrase is deliberately NOT a property here
/// (PasswordBox.Password isn't data-bindable in WinUI) — MainPage reads/writes it
/// directly on the relevant methods below.
/// </summary>
public sealed class MainViewModel : ObservableObject, IDisposable
{
    private readonly CaptureCoreService _captureCore = new();
    private readonly PresetService _presets = new();

    public bool IsNativeCoreAvailable => _captureCore.IsAvailable;

    public ObservableCollection<CaptureDeviceInfo> Devices { get; } = new();
    public ObservableCollection<Preset> Presets { get; } = new();

    public MainViewModel()
    {
        if (!IsNativeCoreAvailable)
        {
            _statusText = "CaptureCore.dll not found next to the app — build native/ first (see build.ps1). Device/streaming controls are disabled until then.";
        }
    }

    private CaptureDeviceInfo? _selectedDevice;
    public CaptureDeviceInfo? SelectedDevice
    {
        get => _selectedDevice;
        set => SetProperty(ref _selectedDevice, value);
    }

    private Preset? _selectedPreset;
    public Preset? SelectedPreset
    {
        get => _selectedPreset;
        set => SetProperty(ref _selectedPreset, value);
    }

    // --- Encode settings (bound via NumberBox/ComboBox/TextBox) ---
    private string _encoderImpl = "libx264";
    public string EncoderImpl { get => _encoderImpl; set => SetProperty(ref _encoderImpl, value); }

    private string _rateControl = "cbr";
    public string RateControl { get => _rateControl; set => SetProperty(ref _rateControl, value); }

    private double _bitrateKbps = 6000;
    public double BitrateKbps { get => _bitrateKbps; set => SetProperty(ref _bitrateKbps, value); }

    private double _maxBitrateKbps = 6000;
    public double MaxBitrateKbps { get => _maxBitrateKbps; set => SetProperty(ref _maxBitrateKbps, value); }

    private double _bufferSizeKbps = 12000;
    public double BufferSizeKbps { get => _bufferSizeKbps; set => SetProperty(ref _bufferSizeKbps, value); }

    private double _keyframeIntervalSec = 2;
    public double KeyframeIntervalSec { get => _keyframeIntervalSec; set => SetProperty(ref _keyframeIntervalSec, value); }

    private string _x264Preset = "veryfast";
    public string X264Preset { get => _x264Preset; set => SetProperty(ref _x264Preset, value); }

    private double _outputWidth = 1920;
    public double OutputWidth { get => _outputWidth; set => SetProperty(ref _outputWidth, value); }

    private double _outputHeight = 1080;
    public double OutputHeight { get => _outputHeight; set => SetProperty(ref _outputHeight, value); }

    private double _outputFrameRate = 30;
    public double OutputFrameRate { get => _outputFrameRate; set => SetProperty(ref _outputFrameRate, value); }

    // --- SRT settings ---
    private string _srtMode = "caller";
    public string SrtMode { get => _srtMode; set => SetProperty(ref _srtMode, value); }

    private string _host = "";
    public string Host { get => _host; set => SetProperty(ref _host, value); }

    private double _port = 9000;
    public double Port { get => _port; set => SetProperty(ref _port, value); }

    private double _latencyMs = 200;
    public double LatencyMs { get => _latencyMs; set => SetProperty(ref _latencyMs, value); }

    private double _pbKeyLen;
    public double PbKeyLen { get => _pbKeyLen; set => SetProperty(ref _pbKeyLen, value); }

    private string _streamId = "";
    public string StreamId { get => _streamId; set => SetProperty(ref _streamId, value); }

    // --- Status ---
    private string _statusText = "No device open.";
    public string StatusText { get => _statusText; set => SetProperty(ref _statusText, value); }

    private bool _isDeviceOpen;
    public bool IsDeviceOpen { get => _isDeviceOpen; private set => SetProperty(ref _isDeviceOpen, value); }

    private bool _isStreaming;
    public bool IsStreaming { get => _isStreaming; private set => SetProperty(ref _isStreaming, value); }

    private StreamStats _stats = Models.StreamStats.Idle;
    public StreamStats Stats { get => _stats; private set => SetProperty(ref _stats, value); }

    public int OpenWidth => _captureCore.OpenWidth;
    public int OpenHeight => _captureCore.OpenHeight;

    public void RefreshDevices()
    {
        Devices.Clear();
        foreach (var device in _captureCore.EnumerateDevices())
        {
            Devices.Add(device);
        }
    }

    public void RefreshPresets()
    {
        var selectedId = SelectedPreset?.Id;
        Presets.Clear();
        foreach (var preset in _presets.ListPresets())
        {
            Presets.Add(preset);
        }
        SelectedPreset = Presets.FirstOrDefault(p => p.Id == selectedId);
    }

    public bool OpenSelectedDevice()
    {
        if (SelectedDevice is null)
        {
            StatusText = "Select a device first.";
            return false;
        }

        _captureCore.CloseSource();
        var opened = _captureCore.OpenSource(SelectedDevice, (int)OutputWidth, (int)OutputHeight, 30, 1);
        IsDeviceOpen = opened;
        StatusText = opened
            ? $"Opened {SelectedDevice.DisplayName} at {_captureCore.OpenWidth}x{_captureCore.OpenHeight}."
            : $"Failed to open {SelectedDevice.DisplayName}.";
        return opened;
    }

    public void CloseDevice()
    {
        _captureCore.CloseSource();
        IsDeviceOpen = false;
        StatusText = "No device open.";
    }

    public bool TryGetPreviewFrame(byte[] buffer) => IsDeviceOpen && _captureCore.TryGetLatestFrame(buffer);

    public bool StartStreaming(string plaintextPassphrase)
    {
        if (!IsDeviceOpen)
        {
            StatusText = "Open a device before streaming.";
            return false;
        }

        var started = _captureCore.StartStream(BuildPresetEncode(), BuildPresetSrt(), plaintextPassphrase);
        IsStreaming = started;
        StatusText = started ? "Streaming started." : "Failed to start streaming (check ffmpeg.exe is present — see tools/ffmpeg).";
        return started;
    }

    public void StopStreaming()
    {
        _captureCore.StopStream();
        IsStreaming = false;
        Stats = Models.StreamStats.Idle;
        StatusText = "Streaming stopped.";
    }

    public void RefreshStats()
    {
        if (!_captureCore.IsStreaming && IsStreaming)
        {
            // ffmpeg exited on its own (crash, connection refused, ...).
            IsStreaming = false;
        }
        Stats = _captureCore.GetStats();
    }

    // Note: the SRT passphrase is intentionally not restored into any bindable field —
    // PasswordBox is left blank on load. Re-entering it is required to change it;
    // leaving it blank on the next Save keeps the existing encrypted value (see
    // SaveAsPreset), which is why it doesn't need to round-trip through the UI at all.
    public void ApplyPreset(Preset preset)
    {
        // Device selection: match by DeviceId if the device is currently enumerated,
        // otherwise leave SelectedDevice unset (device may be unplugged).
        SelectedDevice = Devices.FirstOrDefault(d => d.DeviceId == preset.Source.DeviceId);

        EncoderImpl = preset.Encode.EncoderImpl;
        RateControl = preset.Encode.RateControl;
        BitrateKbps = preset.Encode.BitrateKbps;
        MaxBitrateKbps = preset.Encode.MaxBitrateKbps;
        BufferSizeKbps = preset.Encode.BufferSizeKbps;
        KeyframeIntervalSec = preset.Encode.KeyframeIntervalSec;
        X264Preset = preset.Encode.X264Preset;
        OutputWidth = preset.Encode.OutputWidth;
        OutputHeight = preset.Encode.OutputHeight;
        OutputFrameRate = preset.Encode.OutputFrameRateNumerator / (double)Math.Max(1, preset.Encode.OutputFrameRateDenominator);

        SrtMode = preset.Srt.Mode;
        Host = preset.Srt.Host;
        Port = preset.Srt.Port;
        LatencyMs = preset.Srt.LatencyMs;
        PbKeyLen = preset.Srt.PbKeyLen;
        StreamId = preset.Srt.StreamId;
    }

    public Preset SaveAsPreset(string name, string? existingId, string? plaintextPassphrase)
    {
        var preset = existingId is not null
            ? Presets.FirstOrDefault(p => p.Id == existingId) ?? new Preset { Id = existingId }
            : new Preset();

        preset.Name = name;
        preset.Source = new PresetSource
        {
            Backend = SelectedDevice?.Backend ?? "UVC",
            DeviceId = SelectedDevice?.DeviceId ?? "",
            DeviceName = SelectedDevice?.DisplayName ?? "",
        };
        preset.Encode = BuildPresetEncode();
        preset.Srt = BuildPresetSrt();

        // Only overwrite the stored (encrypted) passphrase if a new one was entered;
        // an empty PasswordBox on "save" means "keep the existing one".
        if (!string.IsNullOrEmpty(plaintextPassphrase))
        {
            preset.Srt.PassphraseProtected = PresetService.ProtectPassphrase(plaintextPassphrase);
        }

        _presets.Save(preset);
        RefreshPresets();
        SelectedPreset = Presets.FirstOrDefault(p => p.Id == preset.Id);
        return preset;
    }

    public void DeleteSelectedPreset()
    {
        if (SelectedPreset is null)
        {
            return;
        }
        _presets.Delete(SelectedPreset);
        RefreshPresets();
    }

    private PresetEncode BuildPresetEncode() => new()
    {
        EncoderImpl = EncoderImpl,
        RateControl = RateControl,
        BitrateKbps = (int)BitrateKbps,
        MaxBitrateKbps = (int)MaxBitrateKbps,
        BufferSizeKbps = (int)BufferSizeKbps,
        KeyframeIntervalSec = (int)KeyframeIntervalSec,
        X264Preset = X264Preset,
        OutputWidth = (int)OutputWidth,
        OutputHeight = (int)OutputHeight,
        OutputFrameRateNumerator = (int)OutputFrameRate,
        OutputFrameRateDenominator = 1,
    };

    private PresetSrt BuildPresetSrt() => new()
    {
        Mode = SrtMode,
        Host = Host,
        Port = (int)Port,
        LatencyMs = (int)LatencyMs,
        PbKeyLen = (int)PbKeyLen,
        StreamId = StreamId,
        PassphraseProtected = SelectedPreset?.Srt.PassphraseProtected,
    };

    public void Dispose() => _captureCore.Dispose();
}
