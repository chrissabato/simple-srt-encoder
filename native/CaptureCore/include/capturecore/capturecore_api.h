#pragma once

// Flat C ABI surface for CaptureCore.dll, consumed from C# via P/Invoke
// (see ui/SimpleSrtEncoder/Interop/NativeMethods.cs). Structs and primitives only cross
// this boundary, never C++ classes; see capture_types.h for the shared struct/enum
// definitions.

#include "capture_types.h"
#include <stdint.h>

#if defined(_WIN32)
  #if defined(CAPTURECORE_EXPORTS)
    #define CAPTURECORE_API extern "C" __declspec(dllexport)
  #else
    #define CAPTURECORE_API extern "C" __declspec(dllimport)
  #endif
#else
  #define CAPTURECORE_API extern "C"
#endif

typedef void* CaptureCoreHandle;

// Returns a null-terminated UTF-16 build/version string. The caller supplies a buffer
// and its capacity (in UTF-16 code units, including the terminator); returns the number
// of UTF-16 code units written, or the required capacity (including terminator) if the
// buffer was too small.
CAPTURECORE_API int32_t CaptureCore_GetVersionString(wchar_t* buffer, int32_t bufferCapacity);

// --- Lifetime -----------------------------------------------------------------------

CAPTURECORE_API CaptureCoreHandle CaptureCore_Create();
CAPTURECORE_API void CaptureCore_Destroy(CaptureCoreHandle handle);

// --- Device enumeration ---------------------------------------------------------------

// True if this build was compiled with the given backend's SDK available
// (see ENABLE_DECKLINK/ENABLE_NDI in native/CaptureCore/CMakeLists.txt).
CAPTURECORE_API int32_t CaptureCore_IsBackendAvailable(CcBackendType backend);

// Fills outArray with up to maxCount devices across all available backends. Returns the
// number of devices found, which may exceed maxCount (call again with a larger array
// sized to the returned count if so).
CAPTURECORE_API int32_t CaptureCore_EnumerateDevices(CaptureCoreHandle handle, CcDeviceInfo* outArray, int32_t maxCount);

// Fills outArray with up to maxCount active WASAPI audio capture endpoints (mics/line-in).
// Same overflow convention as CaptureCore_EnumerateDevices.
CAPTURECORE_API int32_t CaptureCore_EnumerateAudioDevices(CaptureCoreHandle handle, CcAudioDeviceInfo* outArray, int32_t maxCount);

// --- Audio loudness monitor -----------------------------------------------------------
// Meters the chosen audio device independently of streaming so levels are visible before
// going live. Pass CC_EMBEDDED_AUDIO_DEVICE_ID for embedded source audio, which can only be
// metered while streaming (its buffer has a single consumer). Streaming start resets the meter.

CAPTURECORE_API int32_t CaptureCore_StartAudioMonitor(CaptureCoreHandle handle, const CcDeviceId* id);
CAPTURECORE_API void CaptureCore_StopAudioMonitor(CaptureCoreHandle handle);
CAPTURECORE_API int32_t CaptureCore_GetLoudness(CaptureCoreHandle handle, CcLoudness* outLoudness);
CAPTURECORE_API void CaptureCore_ResetLoudness(CaptureCoreHandle handle);

// --- Capture source ---------------------------------------------------------------

CAPTURECORE_API int32_t CaptureCore_OpenSource(CaptureCoreHandle handle, const CcDeviceId* id, const CcCaptureFormat* format);
CAPTURECORE_API void CaptureCore_CloseSource(CaptureCoreHandle handle);

// The actually-negotiated frame size, which may differ from what was requested in
// OpenSource (Media Foundation picks the closest native format). Callers must size
// preview/stream buffers from this, not from the originally requested CcCaptureFormat.
CAPTURECORE_API int32_t CaptureCore_GetOpenSourceSize(CaptureCoreHandle handle, int32_t* outWidth, int32_t* outHeight);

// Copies the most recently captured frame (BGRA32) into outFrame->data. outFrame->data
// and outFrame->capacity must already be set by the caller. Returns 0 if no frame is
// available yet (e.g. immediately after OpenSource), or if capacity is too small.
CAPTURECORE_API int32_t CaptureCore_TryGetLatestFrame(CaptureCoreHandle handle, CcFrameBuffer* outFrame);

// --- Encode / SRT streaming ---------------------------------------------------------------

CAPTURECORE_API int32_t CaptureCore_StartStream(CaptureCoreHandle handle, const CcEncodeSettings* encode, const CcSrtSettings* srt);
CAPTURECORE_API void CaptureCore_StopStream(CaptureCoreHandle handle);
CAPTURECORE_API int32_t CaptureCore_IsStreaming(CaptureCoreHandle handle);

// Polled (not pushed) to avoid cross-boundary callback/GC lifetime issues at high
// frequency; call from a UI timer at a few Hz.
CAPTURECORE_API int32_t CaptureCore_GetStreamStats(CaptureCoreHandle handle, CcStreamStats* outStats);

// Runs a tiny real test-encode through the named ffmpeg encoder (e.g. "h264_nvenc") to
// check it's actually usable on this machine right now — catches runtime-only failures
// (GPU driver too old for the required hardware-encode API version, no such GPU present,
// ...) that a compile-time "does ffmpeg support this encoder name" check can't see.
// Result is cached per (ffmpegExeName, encoderName) pair for this handle's lifetime. May
// take up to ~5s the first time it's asked about a given pair; near-instant on repeat
// calls — don't call from a latency-sensitive path, and prefer resolving "auto" once per
// stream start rather than per UI refresh. ffmpegExeName selects which ffmpeg\<name>.exe
// to probe (see CcEncodeSettings.ffmpegExeName); null or empty means the default
// "ffmpeg.exe".
CAPTURECORE_API int32_t CaptureCore_ProbeEncoder(CaptureCoreHandle handle, const wchar_t* encoderName, const wchar_t* ffmpegExeName);
