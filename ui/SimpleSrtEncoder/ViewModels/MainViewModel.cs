using System.Collections.ObjectModel;
using SimpleSrtEncoder.Models;
using SimpleSrtEncoder.Services;
using Velopack;

namespace SimpleSrtEncoder.ViewModels;

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
    private readonly UpdateService _updates = new();

    public bool IsNativeCoreAvailable => _captureCore.IsAvailable;

    // "auto" is the portable choice stored in EncoderImpl/presets: it always prefers
    // NVIDIA hardware encoding but falls back automatically on a machine where NVENC
    // isn't usable, so the same preset behaves correctly everywhere instead of
    // hardcoding one machine's capabilities. Resolved to a concrete (encoder, ffmpeg
    // build) pair in ResolveEncoder(), right before actually starting a stream — never
    // persisted as the resolved value.
    public const string AutoEncoder = "auto";

    // ffmpeg ships as two builds (see tools/ffmpeg/fetch-ffmpeg.ps1): the primary build
    // ("" here — FfmpegProcessController defaults an empty name to ffmpeg.exe) tracks
    // ffmpeg master for the newest codecs/fixes, but ffmpeg 9.0 raised NVENC's minimum
    // driver to 610.00+ and broke h264_nvenc on every Pascal-generation NVIDIA GPU
    // (confirmed on a Quadro P2000, driver 582.78 — NVIDIA's R580 branch is the last one
    // that supports Pascal at all, so that's not fixable by updating the driver). The
    // legacy build is pinned to ffmpeg 8.1.x, which only needs NVENC API 13.0 (driver
    // >=570) and works on that same hardware. Auto-resolution below tries NVENC on the
    // primary build first (so newer GPUs still get the newest build) and only reaches
    // for the legacy build as a second attempt, specifically for NVENC — every other
    // encoder always uses the primary build.
    public const string LegacyNvencFfmpeg = "ffmpeg-legacy-nvenc.exe";

    private static readonly (string Encoder, string FfmpegExeName)[] AutoEncoderPreference =
    {
        ("h264_nvenc", ""),
        ("h264_nvenc", LegacyNvencFfmpeg),
        ("h264_qsv", ""),
        ("h264_amf", ""),
        ("libx264", ""),
    };

    // Which ffmpeg builds are worth trying for a given hardware encoder when it's named
    // *explicitly* (not "auto") — e.g. a preset saved before "auto" existed, like one
    // that stores a literal "h264_nvenc" from testing on this exact machine. Without
    // this, an explicit hardware-encoder choice would skip the fallback logic entirely
    // and fail exactly the way "auto" used to before the legacy build existed — real bug
    // hit via a saved preset ("Teela") with EncoderImpl="h264_nvenc" baked in from before
    // this feature shipped.
    private static readonly Dictionary<string, string[]> ExplicitHardwareEncoderBinaries = new(StringComparer.OrdinalIgnoreCase)
    {
        ["h264_nvenc"] = new[] { "", LegacyNvencFfmpeg },
        ["hevc_nvenc"] = new[] { "", LegacyNvencFfmpeg },
        ["h264_qsv"] = new[] { "" },
        ["h264_amf"] = new[] { "" },
    };

    private readonly Dictionary<(string Encoder, string FfmpegExeName), bool> _encoderProbeCache = new();

    // Actually runs a stream's encoder through ffmpeg once (ProbeEncoder launches a real
    // process, up to ~5s worst case) and remembers the result for the rest of this
    // session — hardware availability can't change mid-session, so re-probing on every
    // "auto" resolution would just waste time for no benefit.
    private bool ProbeEncoderCached(string encoder, string ffmpegExeName)
    {
        var key = (encoder, ffmpegExeName);
        if (!_encoderProbeCache.TryGetValue(key, out var available))
        {
            available = _captureCore.ProbeEncoder(encoder, ffmpegExeName);
            _encoderProbeCache[key] = available;
        }
        return available;
    }

    // libx264 (software) is ffmpeg's universal fallback and always assumed available on
    // the primary build — it ships with every ffmpeg build this app uses, so probing it
    // would only add latency for a result that's never actually in question.
    //
    // Ok is false only for an explicitly-named hardware encoder that failed on every
    // bundled ffmpeg build for it — the caller should refuse to start rather than launch
    // ffmpeg knowing it will fail. "auto" always succeeds (falls through to libx264).
    private (string Encoder, string FfmpegExeName, bool Ok) ResolveEncoder(string requested)
    {
        if (string.Equals(requested, AutoEncoder, StringComparison.OrdinalIgnoreCase))
        {
            foreach (var (candidate, exeName) in AutoEncoderPreference)
            {
                if (string.Equals(candidate, "libx264", StringComparison.OrdinalIgnoreCase) || ProbeEncoderCached(candidate, exeName))
                {
                    return (candidate, exeName, true);
                }
            }
            return ("libx264", "", true);
        }

        // Explicit choice — never silently substitute a *different* encoder the user
        // didn't ask for, but a known hardware encoder still gets tried across every
        // bundled ffmpeg build that might support it (see ExplicitHardwareEncoderBinaries)
        // instead of blindly trusting a name that may be stale or wrong for this machine.
        if (ExplicitHardwareEncoderBinaries.TryGetValue(requested, out var binaries))
        {
            foreach (var exeName in binaries)
            {
                if (ProbeEncoderCached(requested, exeName))
                {
                    return (requested, exeName, true);
                }
            }
            return (requested, "", false);
        }

        return (requested, "", true); // software/unrecognized encoder — trust as given, no probe
    }

    public ObservableCollection<CaptureDeviceInfo> Devices { get; } = new();
    public ObservableCollection<CaptureDeviceInfo> AudioDevices { get; } = new();
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
    private string _encoderImpl = AutoEncoder;
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

    // 59.94 (not a round 30/60) default because a mistyped/free-typed frame rate (the
    // "Output frame rate" NumberBox previously let this become e.g. 64 by accident)
    // silently picks the wrong DeckLink display mode with no auto-detection to correct
    // it on cards that don't support format detection — real hardware testing hit
    // exactly this, and 59.94 is the most common broadcast source rate.
    //
    // A dropdown constraining input to FrameRateOptions below (instead of free text
    // entry) was attempted the same day but reverted: x:Bind Mode=TwoWay on
    // ComboBox.SelectedItem against this double property crashed the app on launch
    // (0xc000027b) on the real test machine. A future attempt should populate/read the
    // ComboBox manually in code-behind (MainPage.xaml.cs) instead of through x:Bind, per
    // the pattern already used for DeviceComboBox.
    private double _outputFrameRate = 59.94;
    public double OutputFrameRate { get => _outputFrameRate; set => SetProperty(ref _outputFrameRate, value); }

    // Canonical broadcast frame rates — used by SnapToFrameRateOption below (and ready
    // to back a future frame-rate dropdown; see the comment on OutputFrameRate's default
    // for why that doesn't exist yet).
    public IReadOnlyList<double> FrameRateOptions { get; } = new[] { 23.976, 24.0, 25.0, 29.97, 30.0, 50.0, 59.94, 60.0 };

    // Snaps a computed fps (e.g. from a preset's stored numerator/denominator, which for
    // a drop-frame rate like 60000/1001 divides out to 59.9400599... not exactly 59.94)
    // to the matching FrameRateOptions entry, keeping OutputFrameRate on one of our
    // canonical values so ToFrameRateRational below maps it back to the exact same
    // rational it came from, rather than losing drop-frame precision on a round trip.
    private double SnapToFrameRateOption(double fps)
    {
        foreach (var option in FrameRateOptions)
        {
            if (Math.Abs(option - fps) < 0.01)
            {
                return option;
            }
        }
        return FrameRateOptions.OrderBy(o => Math.Abs(o - fps)).First();
    }

    // Converts a UI-facing fps value to the (numerator, denominator) pair CaptureCore's
    // device-open negotiation expects. Recognizes the common NTSC drop-frame rates
    // (23.976/29.97/59.94, each commonly entered/displayed as the rounded 24/30/60) as
    // their exact broadcast rationals (24000/1001 etc.) rather than truncating them to a
    // plain whole-number/1 rate — real DeckLink hardware (e.g. a source genuinely
    // outputting 1080p59.94) needs the real rational to match against the card's
    // enumerated display modes correctly, not just a close integer approximation.
    private static (int Numerator, int Denominator) ToFrameRateRational(double fps)
    {
        const double epsilon = 0.01;
        if (Math.Abs(fps - 23.976) < epsilon) return (24000, 1001);
        if (Math.Abs(fps - 29.97) < epsilon) return (30000, 1001);
        if (Math.Abs(fps - 59.94) < epsilon) return (60000, 1001);
        if (Math.Abs(fps - 119.88) < epsilon) return (120000, 1001);
        return (Math.Max(1, (int)Math.Round(fps)), 1);
    }

    private bool _audioEnabled;
    public bool AudioEnabled
    {
        get => _audioEnabled;
        set
        {
            if (SetProperty(ref _audioEnabled, value))
            {
                UpdateAudioMonitor();
            }
        }
    }

    private CaptureDeviceInfo? _selectedAudioDevice;
    public CaptureDeviceInfo? SelectedAudioDevice
    {
        get => _selectedAudioDevice;
        set
        {
            if (SetProperty(ref _selectedAudioDevice, value))
            {
                UpdateAudioMonitor();
            }
        }
    }

    private LoudnessReading _loudness = LoudnessReading.Silent;
    public LoudnessReading Loudness { get => _loudness; private set => SetProperty(ref _loudness, value); }

    public void RefreshLoudness() => Loudness = _captureCore.GetLoudness();

    public void ResetLoudness() => _captureCore.ResetLoudness();

    // Matches CC_EMBEDDED_AUDIO_DEVICE_ID in capture_types.h.
    private const string EmbeddedAudioDeviceId = "embedded";

    // Embedded audio (e.g. DeckLink SDI/HDMI audio) has a single consumer — the streaming
    // pump — so CaptureManager::StartAudioMonitor can only configure the meter for it, not
    // feed it PCM, until a stream is actually running. Every other audio device meters
    // independently of streaming (see UpdateAudioMonitor above).
    public bool MeterAwaitingStream =>
        AudioEnabled && SelectedAudioDevice?.DeviceId == EmbeddedAudioDeviceId && !IsStreaming;

    // The meter runs whenever audio is enabled and a device is chosen, not only while
    // streaming, so levels can be checked before going live.
    private void UpdateAudioMonitor()
    {
        _captureCore.StopAudioMonitor();
        Loudness = LoudnessReading.Silent;
        if (!AudioEnabled || SelectedAudioDevice is null)
        {
            return;
        }
        if (!_captureCore.StartAudioMonitor(SelectedAudioDevice.DeviceId))
        {
            StatusText = $"Could not open audio device {SelectedAudioDevice.DisplayName} for metering.";
        }
    }

    private double _audioBitrateKbps = 128;
    public double AudioBitrateKbps { get => _audioBitrateKbps; set => SetProperty(ref _audioBitrateKbps, value); }

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

    // --- Update checking (Velopack; no-op entirely when not running from a
    // Velopack-installed copy — see UpdateService.IsAvailable) ---
    private UpdateInfo? _pendingUpdate;

    private bool _updateAvailable;
    public bool UpdateAvailable { get => _updateAvailable; private set => SetProperty(ref _updateAvailable, value); }

    private string _updateStatusText = "";
    public string UpdateStatusText { get => _updateStatusText; private set => SetProperty(ref _updateStatusText, value); }

    public async Task CheckForUpdatesAsync()
    {
        var update = await _updates.CheckForUpdatesAsync();
        if (update is null)
        {
            return;
        }
        _pendingUpdate = update;
        UpdateStatusText = $"Version {update.TargetFullRelease.Version} is available.";
        UpdateAvailable = true;
    }

    public async Task InstallUpdateAndRestartAsync()
    {
        if (_pendingUpdate is null)
        {
            return;
        }
        UpdateStatusText = "Downloading update…";
        await _updates.DownloadAndApplyAsync(_pendingUpdate);
    }

    private bool _isStreaming;
    public bool IsStreaming
    {
        get => _isStreaming;
        private set
        {
            if (SetProperty(ref _isStreaming, value))
            {
                OnPropertyChanged(nameof(CanEditSettings));
            }
        }
    }

    // Encode/audio/SRT settings and presets must not change once ffmpeg is already
    // running with a snapshot of them (StartStream() copies settings in at call time;
    // editing afterward would silently desync the UI from what's actually streaming).
    public bool CanEditSettings => !IsStreaming;

    private StreamStats _stats = Models.StreamStats.Idle;
    public StreamStats Stats { get => _stats; private set => SetProperty(ref _stats, value); }

    public int OpenWidth => _captureCore.OpenWidth;
    public int OpenHeight => _captureCore.OpenHeight;

    public void RefreshDevices()
    {
        var selectedId = SelectedDevice?.DeviceId;
        Devices.Clear();
        foreach (var device in _captureCore.EnumerateDevices())
        {
            Devices.Add(device);
        }
        SelectedDevice = Devices.FirstOrDefault(d => d.DeviceId == selectedId);
    }

    public void RefreshAudioDevices()
    {
        var selectedId = SelectedAudioDevice?.DeviceId;
        AudioDevices.Clear();
        foreach (var device in _captureCore.EnumerateAudioDevices())
        {
            AudioDevices.Add(device);
        }
        SelectedAudioDevice = AudioDevices.FirstOrDefault(d => d.DeviceId == selectedId);
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

    private string? _openDeviceId;
    // Serializes open/close calls into native code — OpenSource can block for several
    // seconds (e.g. DeckLink's format-detection wait), and without this a second
    // selection-changed event firing while that's still in flight would call into the
    // same native CaptureManager handle from two threads at once.
    private readonly SemaphoreSlim _deviceOpenLock = new(1, 1);

    public async Task<bool> OpenSelectedDeviceAsync()
    {
        if (SelectedDevice is null)
        {
            StatusText = "Select a device first.";
            return false;
        }

        // Selecting a device now auto-opens it (see DeviceComboBox_SelectionChanged),
        // which also re-fires when RefreshDevices() swaps in an equal-but-different
        // CaptureDeviceInfo instance for the dropdown's auto-refresh-on-open — skip
        // the close/reopen when it's already the open device.
        if (IsDeviceOpen && _openDeviceId == SelectedDevice.DeviceId)
        {
            return true;
        }

        var device = SelectedDevice;
        StatusText = $"Connecting to {device.DisplayName}...";

        await _deviceOpenLock.WaitAsync();
        try
        {
            // OpenSource (and the CloseSource before it) are blocking native calls — a
            // DeckLink device in particular can take several real seconds (format
            // detection + relock) before returning. Run them off the UI thread so the
            // whole app doesn't freeze for that whole window.
            var (frameRateNumerator, frameRateDenominator) = ToFrameRateRational(OutputFrameRate);
            var (opened, openWidth, openHeight) = await Task.Run(() =>
            {
                _captureCore.CloseSource();
                var ok = _captureCore.OpenSource(
                    device, (int)OutputWidth, (int)OutputHeight, frameRateNumerator, frameRateDenominator);
                return (ok, _captureCore.OpenWidth, _captureCore.OpenHeight);
            });

            // The user may have picked a different device while this was in flight —
            // don't let a slow, now-stale open overwrite the status/state of whatever
            // is actually selected now.
            if (!ReferenceEquals(SelectedDevice, device) && SelectedDevice?.DeviceId != device.DeviceId)
            {
                return opened;
            }

            IsDeviceOpen = opened;
            _openDeviceId = opened ? device.DeviceId : null;
            StatusText = opened
                ? $"Opened {device.DisplayName} at {openWidth}x{openHeight}."
                : $"Failed to open {device.DisplayName}.";
            return opened;
        }
        finally
        {
            _deviceOpenLock.Release();
        }
    }

    public void CloseDevice()
    {
        _captureCore.CloseSource();
        IsDeviceOpen = false;
        _openDeviceId = null;
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

        if (AudioEnabled && SelectedAudioDevice is null)
        {
            StatusText = "Select an audio device, or turn audio off, before streaming.";
            Stats = Stats with { ConnectionState = ConnectionState.Broken };
            return false;
        }

        if (string.IsNullOrWhiteSpace(Host))
        {
            // Real bug hit: an empty Host produced "srt://:9000?..." — ffmpeg then
            // failed with a generic SRT I/O error with no indication the URL itself was
            // malformed. Catch the actually-common cause (no preset applied, or Host
            // cleared) before ever launching ffmpeg.
            StatusText = "Enter an SRT host before streaming (or select a preset that has one).";
            Stats = Stats with { ConnectionState = ConnectionState.Broken };
            return false;
        }

        // BuildPresetEncode() carries the portable preference (e.g. "auto") — resolved to
        // a concrete, verified-working (encoder, ffmpeg build) pair only here, for the
        // native call, never for what gets persisted (see SaveAsPreset, which builds its
        // own unresolved copy).
        var encode = BuildPresetEncode();
        var requestedAuto = string.Equals(encode.EncoderImpl, AutoEncoder, StringComparison.OrdinalIgnoreCase);
        var (resolvedEncoder, ffmpegExeName, ok) = ResolveEncoder(encode.EncoderImpl);
        if (!ok)
        {
            // A named hardware encoder that failed on every bundled ffmpeg build for it —
            // refuse to start rather than launch ffmpeg knowing it will fail silently a
            // few seconds later with no indication why (see the ffmpeg: log lines this
            // exact scenario used to produce with no on-screen notification at all).
            StatusText = $"'{resolvedEncoder}' isn't usable on this machine on any bundled ffmpeg build — switch the encoder to \"auto\" or \"libx264\".";
            Stats = Stats with { ConnectionState = ConnectionState.Broken };
            return false;
        }
        encode.EncoderImpl = resolvedEncoder;

        var started = _captureCore.StartStream(encode, BuildPresetSrt(), plaintextPassphrase, ffmpegExeName);
        IsStreaming = started;
        StatusText = started
            ? requestedAuto || !string.IsNullOrEmpty(ffmpegExeName)
                ? string.IsNullOrEmpty(ffmpegExeName)
                    ? $"Streaming started (auto-selected encoder: {resolvedEncoder})."
                    : $"Streaming started (encoder: {resolvedEncoder}, via legacy ffmpeg build for older-GPU NVENC compatibility)."
                : "Streaming started."
            : "Failed to start streaming (check ffmpeg.exe is present — see tools/ffmpeg).";
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
        Stats = _captureCore.GetStats();
        if (!_captureCore.IsStreaming && IsStreaming)
        {
            // ffmpeg exited on its own (crash, SRT connect refused/timeout, ...) — this
            // is the only place that notices, since nothing else polls native state.
            // Previously this silently flipped IsStreaming without saying why, leaving
            // "Streaming started." shown indefinitely even though nothing was streaming.
            IsStreaming = false;
            StatusText = Stats.ConnectionState == ConnectionState.Broken
                ? "Streaming stopped: ffmpeg exited unexpectedly (SRT connection failed/refused/timed out, or a bad setting). See CaptureCore.log (%TEMP%) for ffmpeg's actual error."
                : "Streaming stopped unexpectedly.";
        }
    }

    // Note: the SRT passphrase is intentionally not restored into any bindable field —
    // PasswordBox is left blank on load. Re-entering it is required to change it;
    // leaving it blank on the next Save keeps the existing encrypted value (see
    // SaveAsPreset), which is why it doesn't need to round-trip through the UI at all.
    public void ApplyPreset(Preset preset)
    {
        // This runs directly off a XAML ListView/ComboBox SelectionChanged event (a WinRT
        // call boundary) — an exception escaping it uncaught fails fast with 0xc000027b
        // instead of surfacing as a normal catchable .NET exception (same class of crash
        // documented elsewhere in this project, and the same fix already applied to
        // SaveAsPreset below after a real "saving a preset crashes the app" report).
        // Confirmed as a real, not just theoretical, risk here too: loading a preset
        // crashed instead of showing an error.
        try
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
            OutputFrameRate = SnapToFrameRateOption(
                preset.Encode.OutputFrameRateNumerator / (double)Math.Max(1, preset.Encode.OutputFrameRateDenominator));

            AudioEnabled = preset.Encode.AudioEnabled;
            AudioBitrateKbps = preset.Encode.AudioBitrateKbps;
            // Match by DeviceId if currently enumerated, otherwise leave unset (device may
            // be unplugged) — same convention as the video SelectedDevice match above.
            SelectedAudioDevice = AudioDevices.FirstOrDefault(d => d.DeviceId == preset.Encode.AudioDeviceId);

            SrtMode = preset.Srt.Mode;
            Host = preset.Srt.Host;
            Port = preset.Srt.Port;
            LatencyMs = preset.Srt.LatencyMs;
            PbKeyLen = preset.Srt.PbKeyLen;
            StreamId = preset.Srt.StreamId;
        }
        catch (Exception ex)
        {
            StatusText = $"Could not load preset \"{preset.Name}\": {ex.GetType().Name}: {ex.Message}";
        }
    }

    // Clears the current selection so the Presets section starts a fresh, unnamed
    // preset rather than overwriting whatever was selected — there's otherwise no way
    // to get back to a "new preset" state once one is selected (ListView/ComboBox
    // selection can't be cleared by clicking).
    public void NewPreset() => SelectedPreset = null;

    public bool SaveAsPreset(string name, string? existingId, string? plaintextPassphrase)
    {
        name = name.Trim();
        if (Presets.Any(p => p.Id != existingId && string.Equals(p.Name, name, StringComparison.CurrentCultureIgnoreCase)))
        {
            StatusText = $"A preset named \"{name}\" already exists — choose a different name.";
            return false;
        }

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

        try
        {
            _presets.Save(preset);
        }
        catch (Exception ex)
        {
            // This handler runs directly off a XAML button-click event (a WinRT call
            // boundary) — an exception escaping it uncaught doesn't surface as a normal
            // catchable .NET exception, it fails fast with 0xc000027b instead (the same
            // class of crash already documented elsewhere in this project for native
            // calls crossing this same kind of boundary). Confirmed as a real, not
            // theoretical, crash: saving a preset on a machine with a different/missing
            // write permission or disk state crashed the whole app instead of showing an
            // error.
            StatusText = $"Could not save preset \"{name}\": {ex.GetType().Name}: {ex.Message}";
            return false;
        }

        var wasUpdate = existingId is not null;
        RefreshPresets();
        SelectedPreset = Presets.FirstOrDefault(p => p.Id == preset.Id);
        StatusText = wasUpdate ? $"Updated preset \"{name}\"." : $"Saved new preset \"{name}\".";
        return true;
    }

    public void DeleteSelectedPreset()
    {
        if (SelectedPreset is null)
        {
            StatusText = "Select a preset to delete first.";
            return;
        }
        var name = SelectedPreset.Name;
        _presets.Delete(SelectedPreset);
        RefreshPresets();
        StatusText = $"Deleted preset \"{name}\".";
    }

    private PresetEncode BuildPresetEncode()
    {
        // Was `(int)OutputFrameRate` / 1 below — truncated a drop-frame rate like 59.94 to
        // a plain 59/1 passed to ffmpeg's `-r`, mislabeling every incoming frame's PTS
        // spacing as 1/59s instead of the true 1/59.94s. Frames actually arrive at the true
        // rate (confirmed in CaptureCore.log: the capture side negotiates the real
        // 59.940060fps DeckLink mode via this same ToFrameRateRational helper), so that
        // 1.6% mislabeling made ffmpeg's video PTS timeline run ~1.6% faster than real
        // elapsed time while audio's PTS (driven by actual sample count/48000) stayed
        // accurate — a steadily growing video-ahead-of-audio drift of about that
        // percentage of elapsed stream time, which is what surfaced as a multi-second A/V
        // desync on a real multi-minute DeckLink stream. Reusing the capture side's helper
        // gets `-r` the exact rational (e.g. 60000/1001) instead of a rounded approximation.
        var (outputFrameRateNumerator, outputFrameRateDenominator) = ToFrameRateRational(OutputFrameRate);
        return new()
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
            OutputFrameRateNumerator = outputFrameRateNumerator,
            OutputFrameRateDenominator = outputFrameRateDenominator,
            AudioEnabled = AudioEnabled,
            AudioDeviceId = SelectedAudioDevice?.DeviceId ?? "",
            AudioDeviceName = SelectedAudioDevice?.DisplayName ?? "",
            AudioBitrateKbps = (int)AudioBitrateKbps,
        };
    }

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
