using System.Runtime.InteropServices;

namespace SrtEncoderApp.Interop;

/// <summary>
/// P/Invoke surface for CaptureCore.dll (native/CaptureCore). Mirrors
/// capturecore_api.h — keep the two in sync by hand, since there is no
/// automatic binding generator in this project.
/// </summary>
internal static partial class NativeMethods
{
    private const string CaptureCoreLibrary = "CaptureCore";

    // ushort, not char: char isn't blittable under LibraryImport's default (safe)
    // marshalling, and ushort matches a native wchar_t (UTF-16 code unit) exactly
    // with no marshalling step needed.
    [LibraryImport(CaptureCoreLibrary, EntryPoint = "CaptureCore_GetVersionString")]
    private static partial int CaptureCore_GetVersionString(
        [Out] ushort[] buffer,
        int bufferCapacity);

    /// <summary>
    /// Calls into CaptureCore.dll and returns its build/version string.
    /// Exists in Phase 0 to prove the native/C# toolchain and P/Invoke
    /// marshaling work end-to-end before any real capture code is written.
    /// </summary>
    public static string GetCaptureCoreVersion()
    {
        var buffer = new ushort[64];
        var required = CaptureCore_GetVersionString(buffer, buffer.Length);

        if (required > buffer.Length)
        {
            buffer = new ushort[required];
            required = CaptureCore_GetVersionString(buffer, buffer.Length);
        }

        // required includes the null terminator; exclude it from the managed string.
        var length = Math.Max(0, required - 1);
        return string.Create(length, buffer, static (span, buf) =>
        {
            for (var i = 0; i < span.Length; i++)
            {
                span[i] = (char)buf[i];
            }
        });
    }

    // The remaining calls carry structs with embedded fixed-size strings
    // ([MarshalAs(UnmanagedType.ByValTStr)] in NativeStructs.cs), which the
    // LibraryImport source generator doesn't support — classic DllImport marshaling
    // handles these natively and is the better fit here.

    [DllImport(CaptureCoreLibrary, CallingConvention = CallingConvention.Cdecl)]
    public static extern nint CaptureCore_Create();

    [DllImport(CaptureCoreLibrary, CallingConvention = CallingConvention.Cdecl)]
    public static extern void CaptureCore_Destroy(nint handle);

    [DllImport(CaptureCoreLibrary, CallingConvention = CallingConvention.Cdecl)]
    public static extern int CaptureCore_IsBackendAvailable(CcBackendType backend);

    [DllImport(CaptureCoreLibrary, CallingConvention = CallingConvention.Cdecl)]
    public static extern int CaptureCore_EnumerateDevices(nint handle, [Out] CcDeviceInfo[] outArray, int maxCount);

    [DllImport(CaptureCoreLibrary, CallingConvention = CallingConvention.Cdecl)]
    public static extern int CaptureCore_OpenSource(nint handle, in CcDeviceId id, in CcCaptureFormat format);

    [DllImport(CaptureCoreLibrary, CallingConvention = CallingConvention.Cdecl)]
    public static extern void CaptureCore_CloseSource(nint handle);

    [DllImport(CaptureCoreLibrary, CallingConvention = CallingConvention.Cdecl)]
    public static extern int CaptureCore_GetOpenSourceSize(nint handle, out int outWidth, out int outHeight);

    [DllImport(CaptureCoreLibrary, CallingConvention = CallingConvention.Cdecl)]
    public static extern int CaptureCore_TryGetLatestFrame(nint handle, ref CcFrameBuffer outFrame);

    [DllImport(CaptureCoreLibrary, CallingConvention = CallingConvention.Cdecl)]
    public static extern int CaptureCore_StartStream(nint handle, in CcEncodeSettings encode, in CcSrtSettings srt);

    [DllImport(CaptureCoreLibrary, CallingConvention = CallingConvention.Cdecl)]
    public static extern void CaptureCore_StopStream(nint handle);

    [DllImport(CaptureCoreLibrary, CallingConvention = CallingConvention.Cdecl)]
    public static extern int CaptureCore_IsStreaming(nint handle);

    [DllImport(CaptureCoreLibrary, CallingConvention = CallingConvention.Cdecl)]
    public static extern int CaptureCore_GetStreamStats(nint handle, out CcStreamStats outStats);
}
