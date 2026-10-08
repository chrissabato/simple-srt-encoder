namespace SimpleSrtEncoder.Models;

public enum ConnectionState
{
    Idle,
    Connecting,
    Connected,
    Broken,
    Stopped,
}

public sealed record StreamStats(
    ConnectionState ConnectionState,
    double BitrateKbps,
    double Fps,
    long DroppedFrames,
    long FramesEncoded)
{
    public static StreamStats Idle { get; } = new(ConnectionState.Idle, 0, 0, 0, 0);
}
