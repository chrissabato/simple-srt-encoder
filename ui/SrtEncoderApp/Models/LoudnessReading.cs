namespace SrtEncoderApp.Models;

/// <summary>EBU R128 readings in LUFS (peak in dBFS); -120 means silence / not yet measurable.</summary>
public sealed record LoudnessReading(double Momentary, double ShortTerm, double Integrated, double PeakDbfs)
{
    public const double Floor = -120.0;

    public static readonly LoudnessReading Silent = new(Floor, Floor, Floor, Floor);
}
