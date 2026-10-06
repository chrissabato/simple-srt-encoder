using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
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
    private readonly DispatcherQueueTimer _meterTimer;

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
        ViewModel.RefreshAudioDevices();
        ViewModel.RefreshPresets();
        _ = ViewModel.CheckForUpdatesAsync();

        var dispatcherQueue = DispatcherQueue.GetForCurrentThread();

        _previewTimer = dispatcherQueue.CreateTimer();
        _previewTimer.Interval = TimeSpan.FromMilliseconds(1000.0 / 30.0);
        _previewTimer.Tick += (_, _) => UpdatePreviewFrame();
        _previewTimer.Start();

        _statsTimer = dispatcherQueue.CreateTimer();
        _statsTimer.Interval = TimeSpan.FromMilliseconds(250);
        _statsTimer.Tick += (_, _) => ViewModel.RefreshStats();
        _statsTimer.Start();

        _meterTimer = dispatcherQueue.CreateTimer();
        _meterTimer.Interval = TimeSpan.FromMilliseconds(100);
        _meterTimer.Tick += (_, _) => UpdateLoudnessMeters();
        _meterTimer.Start();

        Unloaded += (_, _) =>
        {
            _previewTimer.Stop();
            _statsTimer.Stop();
            _meterTimer.Stop();
            ViewModel.Dispose();
        };
    }

    private void UpdateLoudnessMeters()
    {
        var visibility = ViewModel.AudioEnabled ? Visibility.Visible : Visibility.Collapsed;
        PreviewMeter.Visibility = visibility;
        FullscreenMeter.Visibility = visibility;
        if (visibility == Visibility.Collapsed)
        {
            return;
        }

        ViewModel.RefreshLoudness();
        PreviewMeter.Update(ViewModel.Loudness);
        if (FullscreenOverlay.Visibility == Visibility.Visible)
        {
            FullscreenMeter.Update(ViewModel.Loudness);
        }
    }

    private void Meter_ResetRequested(object? sender, EventArgs e) => ViewModel.ResetLoudness();

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
            FullscreenPreviewImage.Source = _previewBitmap;
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

    private void DeviceComboBox_DropDownOpened(object sender, object e) => ViewModel.RefreshDevices();

    private async void DeviceComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (ViewModel.SelectedDevice is not null)
        {
            await ViewModel.OpenSelectedDeviceAsync();
        }
    }

    private void SettingsToggleButton_Click(object sender, RoutedEventArgs e) =>
        SettingsPanel.Visibility = SettingsToggleButton.IsChecked == true ? Visibility.Visible : Visibility.Collapsed;

    private void RefreshAudioDevicesButton_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e) => ViewModel.RefreshAudioDevices();

    private async void InstallUpdateButton_Click(object sender, RoutedEventArgs e) => await ViewModel.InstallUpdateAndRestartAsync();

    private void FullscreenButton_Click(object sender, RoutedEventArgs e) => EnterFullscreen();

    private void PreviewBorder_DoubleTapped(object sender, DoubleTappedRoutedEventArgs e) => EnterFullscreen();

    private void ExitFullscreenButton_Click(object sender, RoutedEventArgs e) => ExitFullscreen();

    private void FullscreenOverlay_DoubleTapped(object sender, DoubleTappedRoutedEventArgs e) => ExitFullscreen();

    private void FullscreenOverlay_KeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key == Windows.System.VirtualKey.Escape)
        {
            ExitFullscreen();
        }
    }

    private void EnterFullscreen()
    {
        FullscreenOverlay.Visibility = Visibility.Visible;
        FullscreenOverlay.Focus(FocusState.Programmatic);
        (App.MainWindow as MainWindow)?.EnterFullscreen();
    }

    private void ExitFullscreen()
    {
        FullscreenOverlay.Visibility = Visibility.Collapsed;
        (App.MainWindow as MainWindow)?.ExitFullscreen();
    }

    private void StreamToggleButton_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        // Content is x:Bind'd to ViewModel.IsStreaming (see MainPage.xaml) rather than
        // set here directly, so it also resets correctly if ffmpeg dies on its own and
        // RefreshStats() flips IsStreaming back off — that used to leave this button
        // stuck reading "Stop Streaming" even though nothing was streaming anymore.
        if (ViewModel.IsStreaming)
        {
            ViewModel.StopStreaming();
        }
        else
        {
            ViewModel.StartStreaming(PassphraseBox.Password);
        }
    }

    private string FormatStreamButtonContent(bool isStreaming) => isStreaming ? "Stop Streaming" : "Start Streaming";

    private bool IsBroken(ConnectionState state) => state == ConnectionState.Broken;

    private void PresetListView_SelectionChanged(object sender, SelectionChangedEventArgs e) => ApplySelectedPresetFrom(e);

    // Mirrors the Presets ListView in the settings panel — both are bound TwoWay to
    // ViewModel.SelectedPreset, so selecting in either one keeps the other's selection in
    // sync automatically; this handler just needs to apply the preset's values once.
    private void PresetComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e) => ApplySelectedPresetFrom(e);

    private void ApplySelectedPresetFrom(SelectionChangedEventArgs e)
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

    private void NewPresetButton_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        ViewModel.NewPreset();
        PresetNameBox.Text = string.Empty;
        PassphraseBox.Password = string.Empty;
    }

    private void SavePresetButton_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        var name = string.IsNullOrWhiteSpace(PresetNameBox.Text) ? "Untitled preset" : PresetNameBox.Text.Trim();
        if (ViewModel.SaveAsPreset(name, ViewModel.SelectedPreset?.Id, PassphraseBox.Password))
        {
            PassphraseBox.Password = string.Empty;
        }
        // On failure (duplicate name) the passphrase is deliberately left in the box —
        // ViewModel.StatusText already explains why, and re-typing a DPAPI-protected
        // passphrase after a rejected save would be an unnecessary extra step.
    }

    private void DeletePresetButton_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        ViewModel.DeleteSelectedPreset();
        PresetNameBox.Text = string.Empty;
    }

    private bool IsNotNull(object? value) => value is not null;

    private string FormatConnectionState(ConnectionState state) => state.ToString();
    private string FormatBitrate(double kbps) => $"{kbps:0} kbps";
    private string FormatFps(double fps) => $"{fps:0.0} fps";
    private string FormatDropped(long dropped) => $"{dropped} dropped";
}
