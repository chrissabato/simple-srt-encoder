#pragma once

#include "capturecore/capture_types.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <windows.h>

namespace capturecore {

// Describes the second ffmpeg input (raw PCM over its own named pipe) when audio is
// enabled. sampleRate/channels/isFloat must exactly match what will actually be written
// to pipeName (see WasapiAudioCapture), since ffmpeg is told this format explicitly via
// -f/-ar/-ac rather than sniffing it.
struct AudioPipeConfig {
    bool enabled = false;
    std::wstring pipeName;
    int32_t sampleRate = 0;
    int32_t channels = 0;
    bool isFloat = false; // true => f32le, false => s16le
};

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

    // Runs a tiny real test-encode (one synthetic frame, no capture source or network
    // needed) through the named ffmpeg encoder and reports whether it actually works
    // right now on this machine. Unlike checking whether ffmpeg was merely *compiled*
    // with an encoder, this catches runtime-only failures — e.g. an NVENC driver too old
    // for the API version this ffmpeg build requires, which is exactly what silently
    // killed a real stream (see CaptureManager::ProbeEncoder for the caching wrapper
    // callers should use instead of calling this directly every time). Blocks the
    // calling thread for up to ~5s in the worst case (hung ffmpeg); callers should not
    // run this on a latency-sensitive thread. exeName selects which ffmpeg\<name>.exe to
    // probe (see FindFfmpegExePath) — empty means the default "ffmpeg.exe".
    static bool ProbeEncoder(const std::wstring& encoderName, const std::wstring& exeName = L"");

    // sourceWidth/sourceHeight describe the raw frames that will be written to pipeName
    // (i.e. the open capture source's dimensions), not the encoder's output resolution.
    // audio.enabled adds a second rawaudio input, muxed alongside the video automatically
    // by ffmpeg's default stream selection (no -map needed: each input contributes
    // exactly one stream of a distinct type).
    bool Start(
        const CcEncodeSettings& encode,
        const CcSrtSettings& srt,
        const std::wstring& pipeName,
        int32_t sourceWidth,
        int32_t sourceHeight,
        const AudioPipeConfig& audio);

    // Requests a graceful stop (CTRL_BREAK, falling back to TerminateProcess), then
    // blocks until the process and its progress-reader thread have both stopped.
    void Stop();

    bool IsRunning() const;
    CcStreamStats GetStats() const;

private:
    void ProgressThreadMain();
    void StderrThreadMain();
    std::wstring BuildCommandLine(
        const CcEncodeSettings& encode,
        const CcSrtSettings& srt,
        const std::wstring& pipeName,
        int32_t sourceWidth,
        int32_t sourceHeight,
        const AudioPipeConfig& audio) const;
    // exeName selects ffmpeg\<exeName> instead of the default ffmpeg\ffmpeg.exe; empty
    // (the default) resolves to "ffmpeg.exe".
    static std::wstring FindFfmpegExePath(const std::wstring& exeName = L"");

    // Closes any still-open process/pipe handles and zeroes the members so a later
    // CreatePipe/CreateProcessW never overwrites a live handle value without closing it
    // first. Shared by Stop() and by the top of Start() — the latter needs this too
    // because a prior ffmpeg that exited on its own (SRT refused, crash, ...) leaves
    // these handles open: nothing calls Stop() in that path (CaptureManager::StartStream
    // calls m_ffmpeg.Start() directly, and the C# side's RefreshStats() only flips a UI
    // flag when it notices ffmpeg died), so Start() used to silently leak 5 handles
    // (hProcess, hThread, and the three pipes) on every such restart.
    void CloseProcessHandles();

    PROCESS_INFORMATION m_processInfo{};
    HANDLE m_stdoutReadPipe = nullptr;
    HANDLE m_stdinWritePipe = nullptr; // for sending ffmpeg's interactive 'q' quit command
    HANDLE m_stderrReadPipe = nullptr; // ffmpeg's -loglevel warning output (real error text
                                        // on SRT connect failures, bad args, etc.) — piped to
                                        // CaptureCore.log rather than discarded to NUL so a
                                        // broken stream is actually diagnosable.
    std::thread m_progressThread;
    std::thread m_stderrThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};

    mutable std::mutex m_statsMutex;
    CcStreamStats m_stats{};
};

} // namespace capturecore
