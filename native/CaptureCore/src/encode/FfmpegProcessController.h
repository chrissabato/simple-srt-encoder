#pragma once

#include "capturecore/capture_types.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <windows.h>

namespace capturecore {

// Launches and supervises ffmpeg.exe as a subprocess: it reads raw BGRA frames from a
// named pipe (see FramePipeWriter) and pushes an encoded stream out over SRT. Chosen
// over linking libav* directly to avoid GPL static-linking entanglement and a large
// vcpkg dependency surface — see docs/architecture.md.
class FfmpegProcessController {
public:
    FfmpegProcessController();
    ~FfmpegProcessController();

    FfmpegProcessController(const FfmpegProcessController&) = delete;
    FfmpegProcessController& operator=(const FfmpegProcessController&) = delete;

    // sourceWidth/sourceHeight describe the raw frames that will be written to pipeName
    // (i.e. the open capture source's dimensions), not the encoder's output resolution.
    bool Start(
        const CcEncodeSettings& encode,
        const CcSrtSettings& srt,
        const std::wstring& pipeName,
        int32_t sourceWidth,
        int32_t sourceHeight);

    // Requests a graceful stop (CTRL_BREAK, falling back to TerminateProcess), then
    // blocks until the process and its progress-reader thread have both stopped.
    void Stop();

    bool IsRunning() const;
    CcStreamStats GetStats() const;

private:
    void ProgressThreadMain();
    std::wstring BuildCommandLine(
        const CcEncodeSettings& encode,
        const CcSrtSettings& srt,
        const std::wstring& pipeName,
        int32_t sourceWidth,
        int32_t sourceHeight) const;
    static std::wstring FindFfmpegExePath();

    PROCESS_INFORMATION m_processInfo{};
    HANDLE m_stdoutReadPipe = nullptr;
    std::thread m_progressThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};

    mutable std::mutex m_statsMutex;
    CcStreamStats m_stats{};
};

} // namespace capturecore
