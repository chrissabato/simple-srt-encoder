using System.Runtime.InteropServices;

namespace SimpleSrtEncoder.Interop;

/// <summary>
/// Hand-written mirrors of native/CaptureCore/include/capturecore/capture_types.h.
/// Keep field order, sizes, and alignment padding in sync by hand — there is no
/// binding generator in this project. Marshaled via classic (DllImport) P/Invoke,
/// which lays out non-blittable structs (those with embedded fixed strings) using the
/// same sequential/natural-alignment rules as the native compiler, so field order here
/// must match the C++ struct exactly, including any alignment gaps (see CcStreamStats).
/// </summary>
internal static class NativeStructs
{
    public const int MaxString = 256;
    public const int MaxShortString = 64;
}

internal enum CcBackendType : int
{
    Uvc = 0,
    DeckLink = 1,
    Ndi = 2,
    DirectShow = 3,
}

internal enum CcPixelFormat : int
{
    Unknown = 0,
    Nv12 = 1,
    Bgra32 = 2,
    Yuy2 = 3,
}

internal enum CcRateControl : int
{
    Cbr = 0,
    Vbr = 1,
}

internal enum CcSrtMode : int
{
    Caller = 0,
    Listener = 1,
    Rendezvous = 2,
}

internal enum CcConnectionState : int
{
    Idle = 0,
    Connecting = 1,
    Connected = 2,
    Broken = 3,
    Stopped = 4,
}

[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
internal struct CcDeviceId
{
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = NativeStructs.MaxString)]
    public string Value;
}

[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
internal struct CcDeviceInfo
{
    public CcBackendType Backend;
    public CcDeviceId Id;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = NativeStructs.MaxString)]
    public string DisplayName;
}

[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
internal struct CcAudioDeviceInfo
{
    public CcDeviceId Id;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = NativeStructs.MaxString)]
    public string DisplayName;
}

[StructLayout(LayoutKind.Sequential)]
internal struct CcRational
{
    public int Numerator;
    public int Denominator;
}

[StructLayout(LayoutKind.Sequential)]
internal struct CcCaptureFormat
{
    public int Width;
    public int Height;
    public CcRational FrameRate;
    public CcPixelFormat PixelFormat;
}

[StructLayout(LayoutKind.Sequential)]
internal struct CcFrameBuffer
{
    public nint Data;
    public int Capacity;
    public int Width;
    public int Height;
    public int StrideBytes;
    public long Timestamp100ns;
}

[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
internal struct CcEncodeSettings
{
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = NativeStructs.MaxShortString)]
    public string EncoderImpl;
    public CcRateControl RateControl;
    public int BitrateKbps;
    public int MaxBitrateKbps;
    public int BufferSizeKbps;
    public int KeyframeIntervalSec;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = NativeStructs.MaxShortString)]
    public string X264Preset;
    public int OutputWidth;
    public int OutputHeight;
    public CcRational OutputFrameRate;
    public int AudioEnabled;
    public int AudioBitrateKbps;
    public CcDeviceId AudioDeviceId;

    // Which ffmpeg\<name>.exe to launch; empty means the default "ffmpeg.exe" — see
    // CaptureCoreService.ProbeEncoder and MainViewModel.ResolveEncoder.
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = NativeStructs.MaxShortString)]
    public string FfmpegExeName;
}

[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
internal struct CcSrtSettings
{
    public CcSrtMode Mode;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = NativeStructs.MaxString)]
    public string Host;
    public int Port;
    public int LatencyMs;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = NativeStructs.MaxString)]
    public string Passphrase;
    public int PbKeyLen;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = NativeStructs.MaxString)]
    public string StreamId;
}

[StructLayout(LayoutKind.Sequential)]
internal struct CcLoudness
{
    public double MomentaryLufs;
    public double ShortTermLufs;
    public double IntegratedLufs;
    public double PeakDbfs;
}

[StructLayout(LayoutKind.Sequential)]
internal struct CcStreamStats
{
    public CcConnectionState ConnectionState;
    // Explicit: `double` after a 4-byte enum needs 8-byte alignment on x64, so both the
    // native (MSVC) and marshaled (CLR interop) layouts insert this gap automatically —
    // spelled out here so the two sides' layouts are visibly, not just implicitly, equal.
    private readonly int _padding;
    public double BitrateKbps;
    public double Fps;
    public long DroppedFrames;
    public long FramesEncoded;
}
