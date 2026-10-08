namespace SimpleSrtEncoder.Models;

public sealed record CaptureDeviceInfo(string Backend, string DeviceId, string DisplayName)
{
    public override string ToString() => DisplayName;
}
