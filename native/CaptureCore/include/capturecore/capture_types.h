#pragma once

// POD types shared across the CaptureCore.dll C ABI (see capturecore_api.h).
// Mirrored by hand in ui/SrtEncoderApp/Interop/NativeStructs.cs — keep the two in sync.
//
// Conventions, since this boundary is marshaled with classic P/Invoke (not a binding
// generator): fixed-size wchar_t[] buffers (not pointers) for strings, no bool (use
// int32_t 0/1 — C++ bool vs. C#'s default 4-byte marshaled bool is a common source of
// ABI bugs), enums are int32_t-sized.

#include <stdint.h>

enum class CcBackendType : int32_t {
    Uvc = 0,
    DeckLink = 1,
    Ndi = 2,
};

enum class CcPixelFormat : int32_t {
    Unknown = 0,
    Nv12 = 1,
    Bgra32 = 2,
    Yuy2 = 3,
};

enum class CcRateControl : int32_t {
    Cbr = 0,
    Vbr = 1,
};

enum class CcSrtMode : int32_t {
    Caller = 0,
    Listener = 1,
    Rendezvous = 2,
};

enum class CcConnectionState : int32_t {
    Idle = 0,
    Connecting = 1,
    Connected = 2,
    Broken = 3,
    Stopped = 4,
};

constexpr int32_t CC_MAX_STRING = 256;
constexpr int32_t CC_MAX_SHORT_STRING = 64;

struct CcDeviceId {
    wchar_t value[CC_MAX_STRING]; // opaque, backend-specific (e.g. an MF symbolic link)
};

struct CcDeviceInfo {
    CcBackendType backend;
    CcDeviceId id;
    wchar_t displayName[CC_MAX_STRING];
};

struct CcRational {
    int32_t numerator;
    int32_t denominator;
};

struct CcCaptureFormat {
    int32_t width;
    int32_t height;
    CcRational frameRate;
    CcPixelFormat pixelFormat;
};

// Caller-owned buffer: the C# side allocates `data`/`capacity` and CaptureCore fills it
// in. `data` must be large enough for width*height*4 bytes (BGRA32); the native side
// never allocates or frees memory that crosses this boundary.
struct CcFrameBuffer {
    void* data;
    int32_t capacity;
    int32_t width;
    int32_t height;
    int32_t strideBytes;
    int64_t timestamp100ns;
};

struct CcEncodeSettings {
    wchar_t encoderImpl[CC_MAX_SHORT_STRING]; // e.g. "libx264", "h264_nvenc"
    CcRateControl rateControl;
    int32_t bitrateKbps;
    int32_t maxBitrateKbps;
    int32_t bufferSizeKbps;
    int32_t keyframeIntervalSec;
    wchar_t x264Preset[CC_MAX_SHORT_STRING]; // e.g. "veryfast"
    int32_t outputWidth;
    int32_t outputHeight;
    CcRational outputFrameRate;
    int32_t audioEnabled; // 0/1
    int32_t audioBitrateKbps;
};

struct CcSrtSettings {
    CcSrtMode mode;
    wchar_t host[CC_MAX_STRING];
    int32_t port;
    int32_t latencyMs;
    wchar_t passphrase[CC_MAX_STRING]; // plaintext; caller decrypts (DPAPI) just before this call
    int32_t pbkeylen; // 0, 16, 24, or 32
    wchar_t streamId[CC_MAX_STRING];
};

struct CcStreamStats {
    CcConnectionState connectionState;
    double bitrateKbps;
    double fps;
    int64_t droppedFrames;
    int64_t framesEncoded;
};
