using SimpleSrtEncoder.Interop;
using SimpleSrtEncoder.Models;

namespace SimpleSrtEncoder.Services;

/// <summary>
/// .NET-friendly wrapper around CaptureCore.dll's C ABI (NativeMethods). Owns one
/// native CaptureManager instance for the app's lifetime.
///
/// Every method below checks <see cref="IsAvailable"/> before calling into native code.
/// If CaptureCore.dll wasn't found at startup (native/ not built yet — see build.ps1),
/// the OS will throw DllNotFoundException on *every* call into it, not just the first;
/// checking IsAvailable turns "native core missing" into a normal degraded UI state
/// (device list stays empty, Open/Start buttons no-op) instead of a crash on first use.
/// </summary>
internal sealed class CaptureCoreService : IDisposable
{
    private readonly CaptureCoreHandle _handle = new();

    public bool IsAvailable => !_handle.IsInvalid;

    public int OpenWidth { get; private set; }
    public int OpenHeight { get; private set; }

    public IReadOnlyList<CaptureDeviceInfo> EnumerateDevices()
    {
        if (!IsAvailable)
        {
            return Array.Empty<CaptureDeviceInfo>();
        }

        var handle = _handle.DangerousGetHandle();

        var count = NativeMethods.CaptureCore_EnumerateDevices(handle, Array.Empty<CcDeviceInfo>(), 0);
        if (count <= 0)
        {
            return Array.Empty<CaptureDeviceInfo>();
        }

        var native = new CcDeviceInfo[count];
        var actual = NativeMethods.CaptureCore_EnumerateDevices(handle, native, native.Length);
        var resultCount = Math.Min(actual, native.Length);

        var result = new List<CaptureDeviceInfo>(resultCount);
        for (var i = 0; i < resultCount; i++)
        {
            result.Add(new CaptureDeviceInfo(native[i].Backend.ToString(), native[i].Id.Value, native[i].DisplayName));
        }
        return result;
    }

    public IReadOnlyList<CaptureDeviceInfo> EnumerateAudioDevices()
    {
        if (!IsAvailable)
        {
            return Array.Empty<CaptureDeviceInfo>();
        }

        var handle = _handle.DangerousGetHandle();

        var count = NativeMethods.CaptureCore_EnumerateAudioDevices(handle, Array.Empty<CcAudioDeviceInfo>(), 0);
        if (count <= 0)
        {
            return Array.Empty<CaptureDeviceInfo>();
        }

        var native = new CcAudioDeviceInfo[count];
        var actual = NativeMethods.CaptureCore_EnumerateAudioDevices(handle, native, native.Length);
        var resultCount = Math.Min(actual, native.Length);

        var result = new List<CaptureDeviceInfo>(resultCount);
        for (var i = 0; i < resultCount; i++)
        {
            result.Add(new CaptureDeviceInfo("Wasapi", native[i].Id.Value, native[i].DisplayName));
        }
        return result;
    }

    public bool StartAudioMonitor(string deviceId)
    {
        if (!IsAvailable)
        {
            return false;
        }
        var id = new CcDeviceId { Value = TruncateForNativeBuffer(deviceId, NativeStructs.MaxString) };
        return NativeMethods.CaptureCore_StartAudioMonitor(_handle.DangerousGetHandle(), in id) != 0;
    }

    public void StopAudioMonitor()
    {
        if (IsAvailable)
        {
            NativeMethods.CaptureCore_StopAudioMonitor(_handle.DangerousGetHandle());
        }
    }

    public LoudnessReading GetLoudness()
    {
        if (!IsAvailable)
        {
            return LoudnessReading.Silent;
        }
        NativeMethods.CaptureCore_GetLoudness(_handle.DangerousGetHandle(), out var native);
        return new LoudnessReading(native.MomentaryLufs, native.ShortTermLufs, native.IntegratedLufs, native.PeakDbfs);
    }

    public void ResetLoudness()
    {
        if (IsAvailable)
        {
            NativeMethods.CaptureCore_ResetLoudness(_handle.DangerousGetHandle());
        }
    }

    public bool IsBackendAvailable(string backend) =>
        IsAvailable &&
        Enum.TryParse<CcBackendType>(backend, ignoreCase: true, out var parsed) &&
        NativeMethods.CaptureCore_IsBackendAvailable(parsed) != 0;

    // ByValTStr-marshaled fields are fixed-size native buffers (see NativeStructs.cs);
    // truncate defensively rather than let an oversized string throw a
    // MarshalDirectiveException deep inside a P/Invoke call. -1 leaves room for the
    // marshaler's own null terminator within the fixed buffer.
    private static string TruncateForNativeBuffer(string value, int sizeConst) =>
        value.Length < sizeConst ? value : value[..(sizeConst - 1)];

    public bool OpenSource(CaptureDeviceInfo device, int width, int height, int frameRateNumerator, int frameRateDenominator)
    {
        if (!IsAvailable)
        {
            return false;
        }

        var id = new CcDeviceId { Value = TruncateForNativeBuffer(device.DeviceId, NativeStructs.MaxString) };
        var format = new CcCaptureFormat
        {
            Width = width,
            Height = height,
            FrameRate = new CcRational { Numerator = frameRateNumerator, Denominator = frameRateDenominator },
            PixelFormat = CcPixelFormat.Bgra32,
        };

        var opened = NativeMethods.CaptureCore_OpenSource(_handle.DangerousGetHandle(), in id, in format) != 0;
        if (opened)
        {
            // Media Foundation picks the closest native format, which may differ from
            // what was requested — query what was actually negotiated so preview/stream
            // buffers are sized correctly, rather than trusting the request.
            NativeMethods.CaptureCore_GetOpenSourceSize(_handle.DangerousGetHandle(), out var actualWidth, out var actualHeight);
            OpenWidth = actualWidth;
            OpenHeight = actualHeight;
        }
        return opened;
    }

    public void CloseSource()
    {
        if (IsAvailable)
        {
            NativeMethods.CaptureCore_CloseSource(_handle.DangerousGetHandle());
        }
        OpenWidth = 0;
        OpenHeight = 0;
    }

    /// <summary>
    /// Fills buffer (caller-allocated, exactly width*height*4 bytes, BGRA32) with the
    /// most recently captured frame. False if no frame is available yet.
    /// </summary>
    public unsafe bool TryGetLatestFrame(byte[] buffer)
    {
        if (!IsAvailable)
        {
            return false;
        }
        fixed (byte* ptr = buffer)
        {
            var frame = new CcFrameBuffer
            {
                Data = (nint)ptr,
                Capacity = buffer.Length,
            };
            return NativeMethods.CaptureCore_TryGetLatestFrame(_handle.DangerousGetHandle(), ref frame) != 0;
        }
    }

    /// <param name="plaintextPassphrase">Decrypted just before this call — see PresetService's DPAPI handling. Never logged or persisted by this layer.</param>
    /// <param name="ffmpegExeName">Which ffmpeg\&lt;name&gt;.exe to launch (see MainViewModel.ResolveEncoder); empty for the default "ffmpeg.exe". Not part of PresetEncode — this is a per-machine resolution detail, not something a portable preset should hardcode.</param>
    public bool StartStream(PresetEncode encode, PresetSrt srt, string plaintextPassphrase, string ffmpegExeName = "")
    {
        if (!IsAvailable)
        {
            return false;
        }

        var encodeNative = new CcEncodeSettings
        {
            EncoderImpl = TruncateForNativeBuffer(encode.EncoderImpl, NativeStructs.MaxShortString),
            FfmpegExeName = TruncateForNativeBuffer(ffmpegExeName, NativeStructs.MaxShortString),
            RateControl = string.Equals(encode.RateControl, "vbr", StringComparison.OrdinalIgnoreCase)
                ? CcRateControl.Vbr
                : CcRateControl.Cbr,
            BitrateKbps = encode.BitrateKbps,
            MaxBitrateKbps = encode.MaxBitrateKbps,
            BufferSizeKbps = encode.BufferSizeKbps,
            KeyframeIntervalSec = encode.KeyframeIntervalSec,
            X264Preset = TruncateForNativeBuffer(encode.X264Preset, NativeStructs.MaxShortString),
            OutputWidth = encode.OutputWidth,
            OutputHeight = encode.OutputHeight,
            OutputFrameRate = new CcRational
            {
                Numerator = encode.OutputFrameRateNumerator,
                Denominator = encode.OutputFrameRateDenominator,
            },
            AudioEnabled = encode.AudioEnabled ? 1 : 0,
            AudioBitrateKbps = encode.AudioBitrateKbps,
            AudioDeviceId = new CcDeviceId { Value = TruncateForNativeBuffer(encode.AudioDeviceId, NativeStructs.MaxString) },
        };

        var srtNative = new CcSrtSettings
        {
            Mode = srt.Mode.ToLowerInvariant() switch
            {
                "listener" => CcSrtMode.Listener,
                "rendezvous" => CcSrtMode.Rendezvous,
                _ => CcSrtMode.Caller,
            },
            Host = TruncateForNativeBuffer(srt.Host, NativeStructs.MaxString),
            Port = srt.Port,
            LatencyMs = srt.LatencyMs,
            Passphrase = TruncateForNativeBuffer(plaintextPassphrase ?? string.Empty, NativeStructs.MaxString),
            PbKeyLen = srt.PbKeyLen,
            StreamId = TruncateForNativeBuffer(srt.StreamId, NativeStructs.MaxString),
        };

        return NativeMethods.CaptureCore_StartStream(_handle.DangerousGetHandle(), in encodeNative, in srtNative) != 0;
    }

    public void StopStream()
    {
        if (IsAvailable)
        {
            NativeMethods.CaptureCore_StopStream(_handle.DangerousGetHandle());
        }
    }

    public bool IsStreaming => IsAvailable && NativeMethods.CaptureCore_IsStreaming(_handle.DangerousGetHandle()) != 0;

    /// <summary>Runs a real (tiny, synthetic) test-encode through the named ffmpeg\&lt;ffmpegExeName&gt;.exe build (empty for the default "ffmpeg.exe") to check the encoder actually works on this machine right now — see CaptureCore_ProbeEncoder. Can take a few seconds; cache the result rather than calling this on every UI refresh.</summary>
    public bool ProbeEncoder(string encoderName, string ffmpegExeName = "") =>
        IsAvailable && NativeMethods.CaptureCore_ProbeEncoder(_handle.DangerousGetHandle(), encoderName, ffmpegExeName) != 0;

    public StreamStats GetStats()
    {
        if (!IsAvailable)
        {
            return Models.StreamStats.Idle;
        }
        NativeMethods.CaptureCore_GetStreamStats(_handle.DangerousGetHandle(), out var native);
        return new StreamStats(
            (ConnectionState)native.ConnectionState,
            native.BitrateKbps,
            native.Fps,
            native.DroppedFrames,
            native.FramesEncoded);
    }

    public void Dispose()
    {
        StopStream();
        CloseSource();
        _handle.Dispose();
    }
}
