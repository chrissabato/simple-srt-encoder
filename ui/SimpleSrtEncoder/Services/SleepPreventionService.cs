using System.Runtime.InteropServices;

namespace SimpleSrtEncoder.Services;

/// <summary>
/// Keeps the machine awake and the display on for the duration of a stream via
/// kernel32's SetThreadExecutionState, so an idle timeout doesn't sleep the PC or
/// blank the screen mid-stream. The flags only apply while continuously renewed
/// (Windows clears them the moment any thread in the process calls this again
/// without ES_CONTINUOUS, or the process exits), so Stop() restores normal idle
/// behavior rather than relying on process exit to clean up.
/// </summary>
internal static class SleepPreventionService
{
    [Flags]
    private enum ExecutionState : uint
    {
        EsContinuous = 0x80000000,
        EsSystemRequired = 0x00000001,
        EsDisplayRequired = 0x00000002,
    }

    [DllImport("kernel32.dll")]
    private static extern ExecutionState SetThreadExecutionState(ExecutionState esFlags);

    public static void Set(bool keepAwake)
    {
        SetThreadExecutionState(keepAwake
            ? ExecutionState.EsContinuous | ExecutionState.EsSystemRequired | ExecutionState.EsDisplayRequired
            : ExecutionState.EsContinuous);
    }
}
