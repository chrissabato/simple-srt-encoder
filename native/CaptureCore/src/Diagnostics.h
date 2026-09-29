#pragma once

// Minimal debug-only diagnostic logging (%TEMP%\CaptureCore.log), for diagnosing
// hardware/driver-specific capture issues on machines with no attached debugger.
// Not part of the public ABI; not intended to stay this verbose long-term — trim or
// gate behind a verbosity flag once the UVC capture path has been validated against a
// reasonable range of real devices.

#include <string>

namespace capturecore {

void LogDiagnostic(const std::wstring& message);

} // namespace capturecore
