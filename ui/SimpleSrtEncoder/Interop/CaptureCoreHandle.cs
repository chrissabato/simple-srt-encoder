using Microsoft.Win32.SafeHandles;

namespace SimpleSrtEncoder.Interop;

/// <summary>
/// Owns the lifetime of one native CaptureManager instance (CaptureCore_Create /
/// CaptureCore_Destroy), so it's always released even if disposal is missed.
///
/// CaptureCore.dll may not be present yet (native/ hasn't been built — see build.ps1),
/// in which case CaptureCore_Create() throws DllNotFoundException. That must be caught
/// here, not left to propagate: an exception thrown this early (during a Page's field
/// initializers, ahead of InitializeComponent) crosses a WinRT/XAML navigation boundary
/// that does not marshal it as a catchable .NET exception — it surfaces as an
/// unrecoverable native crash (0xc000027b) instead. IsInvalid staying true is how
/// CaptureCoreService detects "native core unavailable" and degrades gracefully.
/// </summary>
internal sealed class CaptureCoreHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    public CaptureCoreHandle() : base(ownsHandle: true)
    {
        try
        {
            SetHandle(NativeMethods.CaptureCore_Create());
        }
        catch (DllNotFoundException)
        {
            // Leave the handle at its default (zero/invalid).
        }
    }

    protected override bool ReleaseHandle()
    {
        if (handle != 0)
        {
            NativeMethods.CaptureCore_Destroy(handle);
        }
        return true;
    }
}
