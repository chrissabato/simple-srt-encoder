using System.Collections.Generic;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using SimpleSrtEncoder.Models;
using Windows.Foundation;
using Windows.UI;

namespace SimpleSrtEncoder;

public sealed partial class LoudnessMeterControl : UserControl
{
    private const double TargetLufs = -24.0;
    private const double HistoryMinLufs = -50.0;
    private const double HistoryMaxLufs = 0.0;
    private const int HistoryCapacity = 300; // 30s at the 100ms meter-timer tick in MainPage.xaml.cs

    private static readonly SolidColorBrush QuietBrush = new(Color.FromArgb(0xFF, 0x6C, 0x8E, 0xAD));
    private static readonly SolidColorBrush GoodBrush = new(Color.FromArgb(0xFF, 0x3F, 0xC1, 0x6E));
    private static readonly SolidColorBrush HotBrush = new(Color.FromArgb(0xFF, 0xF2, 0xB1, 0x34));
    private static readonly SolidColorBrush ClipBrush = new(Color.FromArgb(0xFF, 0xE5, 0x48, 0x4D));
    private static readonly SolidColorBrush WhiteBrush = new(Colors.White);

    private readonly Queue<double> _history = new(HistoryCapacity);

    public event EventHandler? ResetRequested;

    public LoudnessMeterControl()
    {
        InitializeComponent();
    }

    public void SetAwaitingStream(bool awaiting) =>
        AwaitingStreamOverlay.Visibility = awaiting ? Visibility.Visible : Visibility.Collapsed;

    public void Update(LoudnessReading reading)
    {
        MomentaryBar.Value = Math.Max(MomentaryBar.Minimum, reading.Momentary);
        MomentaryBar.Foreground = reading.Momentary switch
        {
            <= -50 => QuietBrush,
            > -9 => ClipBrush,
            > -14 => HotBrush,
            _ => GoodBrush,
        };

        MomentaryText.Text = Format(reading.Momentary);
        ShortTermText.Text = Format(reading.ShortTerm);
        IntegratedText.Text = Format(reading.Integrated);
        PeakText.Text = Format(reading.PeakDbfs);
        PeakText.Foreground = reading.PeakDbfs >= -1 ? ClipBrush : WhiteBrush;

        if (_history.Count == HistoryCapacity)
        {
            _history.Dequeue();
        }
        _history.Enqueue(reading.ShortTerm);
        DrawHistory();
    }

    private static string Format(double value) =>
        value <= LoudnessReading.Floor + 0.05 ? "-∞" : value.ToString("0.0");

    private void ResetButton_Click(object sender, RoutedEventArgs e)
    {
        _history.Clear();
        DrawHistory();
        ResetRequested?.Invoke(this, EventArgs.Empty);
    }

    private void HistoryCanvas_SizeChanged(object sender, SizeChangedEventArgs e) => DrawHistory();

    private void DrawHistory()
    {
        var width = HistoryCanvas.ActualWidth;
        var height = HistoryCanvas.ActualHeight;
        if (width <= 0 || height <= 0)
        {
            return;
        }

        var targetY = ValueToY(TargetLufs, height);
        TargetLine.X1 = 0;
        TargetLine.X2 = width;
        TargetLine.Y1 = targetY;
        TargetLine.Y2 = targetY;
        Canvas.SetTop(TargetLabel, Math.Max(0, targetY - 10));
        Canvas.SetLeft(TargetLabel, Math.Max(0, width - 80));

        var points = new PointCollection();
        if (_history.Count > 1)
        {
            var step = width / (HistoryCapacity - 1);
            var startIndex = HistoryCapacity - _history.Count;
            var index = startIndex;
            foreach (var value in _history)
            {
                points.Add(new Point(index * step, ValueToY(value, height)));
                index++;
            }
        }
        HistoryLine.Points = points;
    }

    private static double ValueToY(double lufs, double height)
    {
        var clamped = Math.Clamp(lufs, HistoryMinLufs, HistoryMaxLufs);
        return height * (HistoryMaxLufs - clamped) / (HistoryMaxLufs - HistoryMinLufs);
    }
}
