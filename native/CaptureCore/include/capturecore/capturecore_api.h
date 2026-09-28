#pragma once

// Flat C ABI surface for CaptureCore.dll, consumed from C# via P/Invoke
// (see ui/SrtEncoderApp/Interop/NativeMethods.cs). Kept intentionally small and
// stable: structs and primitives only cross this boundary, never C++ classes.

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

// Phase 0: proves the native <-> C# toolchain and P/Invoke marshaling end-to-end.
// Real capture/encode/preset APIs are added starting Phase 1
// (see docs/architecture.md and the project plan).

// Returns a null-terminated UTF-16 build/version string. The caller supplies a
// buffer and its capacity (in UTF-16 code units, including the terminator);
// returns the number of UTF-16 code units written, or the required capacity
// (including terminator) if the buffer was too small.
CAPTURECORE_API int32_t CaptureCore_GetVersionString(wchar_t* buffer, int32_t bufferCapacity);
