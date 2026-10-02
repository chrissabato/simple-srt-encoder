using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using SrtEncoderApp.Models;
using Windows.UI;

namespace SrtEncoderApp;

public sealed partial class LoudnessMeterControl : UserControl
{
    private static readonly SolidColorBrush QuietBrush = new(Color.FromArgb(0xFF, 0x6C, 0x8E, 0xAD));
    private static readonly SolidColorBrush GoodBrush = new(Color.FromArgb(0xFF, 0x3F, 0xC1, 0x6E));
    private static readonly SolidColorBrush HotBrush = new(Color.FromArgb(0xFF, 0xF2, 0xB1, 0x34));
    private static readonly SolidColorBrush ClipBrush = new(Color.FromArgb(0xFF, 0xE5, 0x48, 0x4D));
    private static readonly SolidColorBrush WhiteBrush = new(Colors.White);

    public event EventHandler? ResetRequested;

    public LoudnessMeterControl()
    {
        InitializeComponent();
    }

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
    }

    private static string Format(double value) =>
        value <= LoudnessReading.Floor + 0.05 ? "-∞" : value.ToString("0.0");

    private void ResetButton_Click(object sender, RoutedEventArgs e) => ResetRequested?.Invoke(this, EventArgs.Empty);
}
