using Microsoft.UI.Xaml.Controls;
using SrtEncoderApp.Interop;

namespace SrtEncoderApp;

/// <summary>
/// The main content page displayed inside the application window.
/// </summary>
public sealed partial class MainPage : Page
{
    public MainPage()
    {
        InitializeComponent();

        // Phase 0 smoke test: prove the CaptureCore.dll <-> WinUI P/Invoke path works.
        try
        {
            NativeCoreStatusText.Text = $"Native core: {NativeMethods.GetCaptureCoreVersion()}";
        }
        catch (DllNotFoundException)
        {
            NativeCoreStatusText.Text =
                "Native core: CaptureCore.dll not found next to the app. " +
                "Build native/ (see build.ps1) before running.";
        }
    }
}
