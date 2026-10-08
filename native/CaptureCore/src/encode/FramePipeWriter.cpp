#include "FramePipeWriter.h"
#include "../Diagnostics.h"

namespace capturecore {

FramePipeWriter::FramePipeWriter(const std::wstring& pipeName) {
    m_connectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_writeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    // One 1080p BGRA32 frame is ~8.3MB; size the pipe buffer generously so a single
    // WriteFile doesn't need to wait on ffmpeg draining it frame-by-frame.
    constexpr DWORD kBufferSize = 16 * 1024 * 1024;

    m_pipe = CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, // max instances
        kBufferSize,
        0,
        0,
        nullptr);

    if (m_pipe == INVALID_HANDLE_VALUE) {
        // Was previously silent: WaitForConnection() just returns false instantly (its
        // own INVALID_HANDLE_VALUE guard) with nothing to explain why, indistinguishable
        // in the log from a real 5s timeout unless someone reads timestamps closely. The
        // most likely cause (confirmed once already) is a leftover FramePipeWriter from a
        // previous session still holding this same pipe name (nMaxInstances=1) — see the
        // reset() calls in CaptureManager::StartStream() that guard against that.
        LogDiagnostic(L"FramePipeWriter: CreateNamedPipeW failed for '" + pipeName +
                      L"', error=" + std::to_wstring(GetLastError()));
    }
}

FramePipeWriter::~FramePipeWriter() {
    if (m_pipe != INVALID_HANDLE_VALUE) {
        CancelIo(m_pipe);
        DisconnectNamedPipe(m_pipe);
        CloseHandle(m_pipe);
    }
    if (m_connectEvent) {
        CloseHandle(m_connectEvent);
    }
    if (m_writeEvent) {
        CloseHandle(m_writeEvent);
    }
}

bool FramePipeWriter::WaitForConnection(DWORD timeoutMs) {
    if (m_pipe == INVALID_HANDLE_VALUE) {
        return false;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = m_connectEvent;

    const BOOL connected = ConnectNamedPipe(m_pipe, &overlapped);
    if (connected) {
        return true; // synchronous connect (rare, but valid)
    }

    switch (GetLastError()) {
        case ERROR_PIPE_CONNECTED:
            return true;
        case ERROR_IO_PENDING: {
            const DWORD waitResult = WaitForSingleObject(m_connectEvent, timeoutMs);
            if (waitResult != WAIT_OBJECT_0) {
                CancelIo(m_pipe);
                return false;
            }
            DWORD bytesTransferred = 0;
            return GetOverlappedResult(m_pipe, &overlapped, &bytesTransferred, FALSE) != 0;
        }
        default:
            return false;
    }
}

bool FramePipeWriter::WriteFrame(const void* data, size_t length) {
    if (m_pipe == INVALID_HANDLE_VALUE) {
        return false;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = m_writeEvent;
    ResetEvent(m_writeEvent);

    DWORD bytesWritten = 0;
    const BOOL ok = WriteFile(m_pipe, data, static_cast<DWORD>(length), &bytesWritten, &overlapped);
    if (!ok) {
        if (GetLastError() != ERROR_IO_PENDING) {
            return false;
        }
        // Bounded wait instead of GetOverlappedResult's own indefinite bWait=TRUE: if
        // ffmpeg stops reading without exiting (e.g. its own write to SRT is stalled on
        // network backpressure, so it never gets back around to draining this pipe), an
        // unbounded wait here would hang the calling streaming/audio thread forever with
        // no way to notice or recover. kWriteTimeoutMs is generous — matching this
        // codebase's other "something's actually wrong by now" timeouts (see
        // WaitForConnection's callers, ProbeEncoder) — so a normal brief stall doesn't
        // false-positive into treating a healthy ffmpeg as dead.
        constexpr DWORD kWriteTimeoutMs = 5000;
        if (WaitForSingleObject(m_writeEvent, kWriteTimeoutMs) != WAIT_OBJECT_0) {
            LogDiagnostic(L"FramePipeWriter: WriteFrame timed out waiting for ffmpeg to read; cancelling");
            CancelIo(m_pipe);
            return false;
        }
        if (!GetOverlappedResult(m_pipe, &overlapped, &bytesWritten, FALSE)) {
            return false;
        }
    }
    return bytesWritten == static_cast<DWORD>(length);
}

} // namespace capturecore
