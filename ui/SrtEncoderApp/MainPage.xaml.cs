using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Imaging;
using SrtEncoderApp.Interop;
using SrtEncoderApp.Models;
using SrtEncoderApp.ViewModels;

namespace SrtEncoderApp;

/// <summary>
/// The main content page. Hosts device selection, live preview, encode/SRT settings,
/// and presets in a single page (see docs/architecture.md — Phase 1 keeps this as one
/// page rather than a multi-view navigation structure not yet justified by the app's
/// size). Preview and stream-stats polling are driven by two DispatcherQueueTimers
/// rather than pushed from native code, per the interop design in NativeMethods.cs.
/// </summary>
public sealed partial class MainPage : Page
{
    public MainViewModel ViewModel { get; } = new();

    private readonly DispatcherQueueTimer _previewTimer;
    private readonly DispatcherQueueTimer _statsTimer;

    private WriteableBitmap? _previewBitmap;
    private byte[]? _previewBuffer;
    private int _previewWidth;
    private int _previewHeight;

    public MainPage()
    {
        InitializeComponent();

        // Phase 0 smoke test, kept as a startup log line rather than removed outright.
        try
        {
            System.Diagnostics.Debug.WriteLine($"Native core: {NativeMethods.GetCaptureCoreVersion()}");
        }
        catch (DllNotFoundException)
        {
            ViewModel.StatusText = "CaptureCore.dll not found next to the app — build native/ first (see build.ps1).";
        }

        ViewModel.RefreshDevices();
        ViewModel.RefreshPresets();

        var dispatcherQueue = DispatcherQueue.GetForCurrentThread();

        _previewTimer = dispatcherQueue.CreateTimer();
        _previewTimer.Interval = TimeSpan.FromMilliseconds(1000.0 / 30.0);
        _previewTimer.Tick += (_, _) => UpdatePreviewFrame();
        _previewTimer.Start();

        _statsTimer = dispatcherQueue.CreateTimer();
        _statsTimer.Interval = TimeSpan.FromMilliseconds(250);
        _statsTimer.Tick += (_, _) => ViewModel.RefreshStats();
        _statsTimer.Start();

        Unloaded += (_, _) =>
        {
            _previewTimer.Stop();
            _statsTimer.Stop();
            ViewModel.Dispose();
        };
    }

    private void UpdatePreviewFrame()
    {
        if (!ViewModel.IsDeviceOpen)
        {
            return;
        }

        var width = ViewModel.OpenWidth;
        var height = ViewModel.OpenHeight;
        if (width <= 0 || height <= 0)
        {
            return;
        }

        if (_previewBitmap is null || _previewWidth != width || _previewHeight != height)
        {
            _previewBitmap = new WriteableBitmap(width, height);
            _previewBuffer = new byte[width * height * 4];
            _previewWidth = width;
            _previewHeight = height;
            PreviewImage.Source = _previewBitmap;
        }

        if (!ViewModel.TryGetPreviewFrame(_previewBuffer!))
        {
            return; // no new frame yet
        }

        // Media Foundation's RGB32 format leaves the 4th (alpha) byte undefined, which
        // WriteableBitmap otherwise renders as fully transparent — force it opaque.
        for (var i = 3; i < _previewBuffer!.Length; i += 4)
        {
            _previewBuffer[i] = 0xFF;
        }

        using (var stream = _previewBitmap.PixelBuffer.AsStream())
        {
            stream.Write(_previewBuffer, 0, _previewBuffer.Length);
        }
        _previewBitmap.Invalidate();
    }

    private void RefreshDevicesButton_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e) => ViewModel.RefreshDevices();

    private void OpenDeviceButton_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e) => ViewModel.OpenSelectedDevice();

    private void CloseDeviceButton_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        ViewModel.CloseDevice();
        PreviewImage.Source = null;
        _previewBitmap = null;
    }

    private void StreamToggleButton_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (ViewModel.IsStreaming)
        {
            ViewModel.StopStreaming();
            StreamToggleButton.Content = "Start Streaming";
        }
        else
        {
            if (ViewModel.StartStreaming(PassphraseBox.Password))
            {
                StreamToggleButton.Content = "Stop Streaming";
            }
        }
    }

    private void PresetListView_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        // Read from the event args rather than ViewModel.SelectedPreset: the x:Bind
        // TwoWay update to that property and this handler both react to the same
        // selection event, and their relative ordering isn't guaranteed.
        if (e.AddedItems.FirstOrDefault() is Preset preset)
        {
            ViewModel.ApplyPreset(preset);
            PresetNameBox.Text = preset.Name;
            PassphraseBox.Password = string.Empty;
        }
    }

    private void SavePresetButton_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        var name = string.IsNullOrWhiteSpace(PresetNameBox.Text) ? "Untitled preset" : PresetNameBox.Text.Trim();
        ViewModel.SaveAsPreset(name, ViewModel.SelectedPreset?.Id, PassphraseBox.Password);
        PassphraseBox.Password = string.Empty;
    }

    private void DeletePresetButton_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e) => ViewModel.DeleteSelectedPreset();

    private string FormatConnectionState(ConnectionState state) => state.ToString();
    private string FormatBitrate(double kbps) => $"{kbps:0} kbps";
    private string FormatFps(double fps) => $"{fps:0.0} fps";
    private string FormatDropped(long dropped) => $"{dropped} dropped";
}
