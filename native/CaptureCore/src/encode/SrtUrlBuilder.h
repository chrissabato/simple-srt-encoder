#pragma once

#include "capturecore/capture_types.h"

#include <string>

namespace capturecore {

// Builds an ffmpeg-compatible srt:// output URL from SRT connection settings.
std::wstring BuildSrtUrl(const CcSrtSettings& srt);

} // namespace capturecore
