#pragma once

#include <cstdint>
#include <string>
#include <windows.h>

namespace capturecore {

// Server side of a Windows named pipe (\\.\pipe\srtencoder_frames_<pid>) that ffmpeg
// connects to as a rawvideo input (`-f rawvideo ... -i \\.\pipe\...`). Decouples frame
// production (capture) from ffmpeg's read rate: a blocked/slow ffmpeg blocks only the
// streaming thread's writes, never the preview path.
class FramePipeWriter {
public:
    explicit FramePipeWriter(const std::wstring& pipeName);
    ~FramePipeWriter();

    FramePipeWriter(const FramePipeWriter&) = delete;
    FramePipeWriter& operator=(const FramePipeWriter&) = delete;

    // Blocks until ffmpeg connects as the reading client, or timeoutMs elapses.
    bool WaitForConnection(DWORD timeoutMs);

    // Writes one full frame. Returns false if the pipe is broken (ffmpeg exited).
    bool WriteFrame(const void* data, size_t length);

private:
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    HANDLE m_connectEvent = nullptr;
    HANDLE m_writeEvent = nullptr;
};

} // namespace capturecore
