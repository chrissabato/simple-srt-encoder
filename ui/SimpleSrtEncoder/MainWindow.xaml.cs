using System.Runtime.InteropServices;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Windows.Graphics;
using WinRT.Interop;

// To learn more about WinUI, the WinUI project structure,
// and more about our project templates, see: http://aka.ms/winui-project-info.

namespace SimpleSrtEncoder;

/// <summary>
/// The application window. This hosts a Frame that displays pages. Add your
/// UI and logic to MainPage.xaml / MainPage.xaml.cs instead of here so you
/// can use Page features such as navigation events and the Loaded lifecycle.
/// </summary>
public sealed partial class MainWindow : Window
{
    // Windows 11 still draws a 1px DWM border around a borderless window; forcing
    // DWMWA_BORDER_COLOR to "none" is the documented way to suppress it.
    private const int DwmwaBorderColor = 34;
    private const uint DwmwaColorNone = 0xFFFFFFFE;
    private const uint DwmwaColorDefault = 0xFFFFFFFF;

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref uint value, int size);

    private const int GwlStyle = -16;
    private const int WsCaption = 0x00C00000;
    private const int WsThickFrame = 0x00040000;
    private const int WsSysMenu = 0x00080000;
    private const int WsMinimizeBox = 0x00020000;
    private const int WsMaximizeBox = 0x00010000;
    private const uint SwpFrameChanged = 0x0020;
    private static readonly IntPtr HwndTopmost = new(-1);
    private static readonly IntPtr HwndNoTopmost = new(-2);

    [DllImport("user32.dll")]
    private static extern int GetWindowLong(IntPtr hWnd, int nIndex);

    [DllImport("user32.dll")]
    private static extern int SetWindowLong(IntPtr hWnd, int nIndex, int dwNewLong);

    [DllImport("user32.dll")]
    private static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter, int x, int y, int cx, int cy, uint uFlags);

    private struct Rect { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll")]
    private static extern bool GetWindowRect(IntPtr hWnd, out Rect rect);

    private Rect? _restoreBounds;
    private int? _restoreStyle;
    // Tracks ExitFullscreen's deferred bounds-restore timer (see its own comment) so a
    // rapid re-entry (double-tap / F11-Esc-F11 within the 100ms delay) can cancel it
    // instead of letting it fire later and fight whatever fullscreen state is active by
    // then — and so EnterFullscreen knows not to re-capture _restoreBounds from the
    // window's current (still fullscreen-sized, not yet visually restored) rect in that
    // window, which previously corrupted _restoreBounds permanently.
    private DispatcherQueueTimer? _pendingRestoreTimer;

    public MainWindow()
    {
        InitializeComponent();

        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);

        AppWindow.SetIcon("Assets/AppIcon.ico");

        // Navigate the root frame to the main page on startup.
        RootFrame.Navigate(typeof(MainPage));
    }

    /// <summary>
    /// Simulates fullscreen with a borderless window resized to the monitor's bounds,
    /// rather than AppWindowPresenterKind.FullScreen: that presenter reserves a 1px
    /// hairline at the top of the screen as a shell-drawn "hover to reveal restore
    /// controls" affordance, which isn't suppressible via app code. The custom title
    /// bar row is hidden too, since it's XAML content in this window's layout and
    /// isn't removed by any presenter change on its own.
    ///
    /// Getting a truly flush top edge also requires clearing WS_CAPTION/WS_THICKFRAME
    /// directly: OverlappedPresenter.SetBorderAndTitleBar(false, false) leaves those
    /// bits set at the Win32 level (confirmed via GetWindowLong), which DWM was still
    /// rendering as a few-pixel frame/shadow once the window sat flush against y=0.
    ///
    /// Bounds and style are saved so ExitFullscreen can restore the exact prior state.
    /// </summary>
    public void EnterFullscreen()
    {
        var hwnd = WindowNative.GetWindowHandle(this);

        if (_pendingRestoreTimer is { } pending)
        {
            // Re-entering fullscreen while ExitFullscreen's deferred bounds-restore is
            // still pending: cancel the stale timer (it would otherwise fire later and
            // forcibly un-fullscreen this new session) and reuse the bounds/style
            // EnterFullscreen already captured the first time, rather than calling
            // GetWindowRect now — at this exact moment the window's style has already
            // been restored (ExitFullscreen does that synchronously, below) but its
            // size/position hasn't (that's the part still pending), so GetWindowRect
            // here would return the still-fullscreen-sized rect and permanently corrupt
            // _restoreBounds with the wrong value.
            pending.Stop();
            _pendingRestoreTimer = null;
        }
        else
        {
            GetWindowRect(hwnd, out var currentBounds);
            _restoreBounds = currentBounds;

            // Deliberately not touching OverlappedPresenter.IsResizable/SetBorderAndTitleBar
            // here: mixing those with the raw GetWindowLong/SetWindowLong/SetWindowPos calls
            // below left AppWindow's internal geometry cache out of sync with the real Win32
            // window, and it kept fighting ExitFullscreen's restore back to these fullscreen
            // bounds (confirmed — every variant of deferring/reordering that restore still
            // lost the race). Plain Win32 for everything, below, avoids that entirely.
            _restoreStyle = GetWindowLong(hwnd, GwlStyle);
        }

        AppTitleBar.Visibility = Visibility.Collapsed;
        SetTitleBar(null);
        AppWindow.TitleBar.SetDragRectangles([]);

        var style = _restoreStyle!.Value & ~(WsCaption | WsThickFrame | WsSysMenu | WsMinimizeBox | WsMaximizeBox);
        SetWindowLong(hwnd, GwlStyle, style);

        // Overshoot the monitor bounds by 1px on every edge. A window whose bounds
        // exactly match the monitor's gets a persistent 1px "hover here to reveal
        // window controls" hint drawn by the shell — confirmed independent of every
        // window-style/DWM-attribute lever tried, so it's a shell heuristic keyed on
        // exact-bounds-match, not anything about this window's own rendering. The
        // 1px overshoot is clipped by the physical screen edge and invisible. Also
        // topmost, since the taskbar is a separate always-on-top shell window that a
        // borderless window covering the monitor doesn't cover on its own.
        var displayArea = DisplayArea.GetFromWindowId(AppWindow.Id, DisplayAreaFallback.Nearest);
        var monitor = displayArea.OuterBounds;
        SetWindowPos(hwnd, HwndTopmost, monitor.X - 1, monitor.Y - 1, monitor.Width + 2, monitor.Height + 2, SwpFrameChanged);

        var noneColor = DwmwaColorNone;
        DwmSetWindowAttribute(hwnd, DwmwaBorderColor, ref noneColor, sizeof(uint));
    }

    public void ExitFullscreen()
    {
        var hwnd = WindowNative.GetWindowHandle(this);

        // Cancel any already-pending restore from a previous ExitFullscreen call rather
        // than letting two timers race to apply the same (harmless but wasteful)
        // restore twice.
        if (_pendingRestoreTimer is { } existingTimer)
        {
            existingTimer.Stop();
            _pendingRestoreTimer = null;
        }

        if (_restoreStyle is { } style)
        {
            SetWindowLong(hwnd, GwlStyle, style);
        }

        var defaultColor = DwmwaColorDefault;
        DwmSetWindowAttribute(hwnd, DwmwaBorderColor, ref defaultColor, sizeof(uint));

        AppTitleBar.Visibility = Visibility.Visible;
        SetTitleBar(AppTitleBar);

        // SetTitleBar(AppTitleBar) above triggers WinAppSDK's own client-area
        // recalculation, which was reasserting this window's (still fullscreen)
        // cached size and undoing a same-tick SetWindowPos restore here — confirmed
        // by the fact that an external SetWindowPos on this hwnd, run right after
        // ExitFullscreen returned, applied and stuck with no fight from anything.
        // A short delay lets that internal recalculation finish first.
        //
        // _restoreBounds/_restoreStyle are deliberately NOT cleared until the timer
        // below actually fires: a rapid re-EnterFullscreen within this 100ms window
        // cancels this timer (via _pendingRestoreTimer) and reuses these saved values
        // instead of capturing fresh (and, at that moment, still-wrong) ones — see
        // EnterFullscreen's comment.
        if (_restoreBounds is { } bounds)
        {
            var timer = DispatcherQueue.CreateTimer();
            timer.Interval = TimeSpan.FromMilliseconds(100);
            timer.IsRepeating = false;
            timer.Tick += (_, _) =>
            {
                SetWindowPos(hwnd, HwndNoTopmost, bounds.Left, bounds.Top, bounds.Right - bounds.Left, bounds.Bottom - bounds.Top, SwpFrameChanged);
                _restoreBounds = null;
                _restoreStyle = null;
                _pendingRestoreTimer = null;
            };
            _pendingRestoreTimer = timer;
            timer.Start();
        }
    }
}
