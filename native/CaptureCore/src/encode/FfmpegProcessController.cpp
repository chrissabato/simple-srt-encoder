#include "FfmpegProcessController.h"
#include "SrtUrlBuilder.h"
#include "../Diagnostics.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <vector>

namespace capturecore {

namespace {

std::wstring GetExecutableDirectory() {
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring fullPath(path, length);
    const size_t lastSlash = fullPath.find_last_of(L"\\/");
    return lastSlash == std::wstring::npos ? L"" : fullPath.substr(0, lastSlash);
}

// Parses one `-progress` block's "key=value" lines (see
// https://ffmpeg.org/ffmpeg.html#Advanced-options, -progress). Called once per
// completed block (terminated by a "progress=continue" or "progress=end" line).
void ApplyProgressLine(const std::wstring& line, CcStreamStats& stats) {
    const size_t eq = line.find(L'=');
    if (eq == std::wstring::npos) {
        return;
    }
    const std::wstring key = line.substr(0, eq);
    const std::wstring value = line.substr(eq + 1);

    try {
        if (key == L"bitrate") {
            // e.g. "6000.0kbits/s" or "N/A"
            if (value != L"N/A") {
                stats.bitrateKbps = std::stod(value);
            }
        } else if (key == L"fps") {
            stats.fps = std::stod(value);
        } else if (key == L"drop_frames") {
            stats.droppedFrames = std::stoll(value);
        } else if (key == L"frame") {
            stats.framesEncoded = std::stoll(value);
        } else if (key == L"progress") {
            stats.connectionState = (value == L"end") ? CcConnectionState::Stopped : CcConnectionState::Connected;
        }
    } catch (const std::exception&) {
        // Malformed/partial line (e.g. truncated read); ignore and wait for the next one.
    }
}

} // namespace

FfmpegProcessController::FfmpegProcessController() = default;

FfmpegProcessController::~FfmpegProcessController() {
    Stop();
}

std::wstring FfmpegProcessController::FindFfmpegExePath(const std::wstring& exeName) {
    return GetExecutableDirectory() + L"\\ffmpeg\\" + (exeName.empty() ? L"ffmpeg.exe" : exeName);
}

bool FfmpegProcessController::ProbeEncoder(const std::wstring& encoderName, const std::wstring& exeName) {
    SECURITY_ATTRIBUTES pipeSecurity{};
    pipeSecurity.nLength = sizeof(SECURITY_ATTRIBUTES);
    pipeSecurity.bInheritHandle = TRUE;

    HANDLE nulHandle = CreateFileW(
        L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &pipeSecurity, OPEN_EXISTING, 0, nullptr);
    if (!nulHandle) {
        return false;
    }

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(STARTUPINFOW);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdInput = nulHandle;
    startupInfo.hStdOutput = nulHandle;
    startupInfo.hStdError = nulHandle;

    // A single synthetic frame through the real encoder — the only reliable way to catch
    // a runtime-only failure (e.g. an NVENC driver too old for this ffmpeg build's
    // required API version). Whether ffmpeg was merely *compiled* with an encoder name
    // says nothing about whether the actual GPU/driver on this machine can use it.
    std::wstringstream cmd;
    cmd << L"\"" << FindFfmpegExePath(exeName) << L"\""
        << L" -hide_banner -loglevel quiet -y"
        << L" -f lavfi -i nullsrc=s=64x64:r=1:d=0.1"
        << L" -frames:v 1 -c:v " << encoderName
        << L" -f null -";

    std::wstring commandLine = cmd.str();
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    PROCESS_INFORMATION processInfo{};
    const BOOL created = CreateProcessW(
        nullptr, mutableCommandLine.data(), nullptr, nullptr, /*bInheritHandles=*/TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startupInfo, &processInfo);
    CloseHandle(nulHandle);

    if (!created) {
        return false;
    }

    // A probe must never hang its caller if ffmpeg somehow wedges; treat a timeout as
    // failure and clean up the stuck process rather than blocking indefinitely.
    const bool exited = WaitForSingleObject(processInfo.hProcess, 5000) == WAIT_OBJECT_0;
    DWORD exitCode = 1;
    if (exited) {
        GetExitCodeProcess(processInfo.hProcess, &exitCode);
    } else {
        TerminateProcess(processInfo.hProcess, 1);
    }
    CloseHandle(processInfo.hProcess);
    CloseHandle(processInfo.hThread);

    return exited && exitCode == 0;
}

std::wstring FfmpegProcessController::BuildCommandLine(
    const CcEncodeSettings& encode,
    const CcSrtSettings& srt,
    const std::wstring& pipeName,
    int32_t sourceWidth,
    int32_t sourceHeight,
    const AudioPipeConfig& audio) const {
    const int32_t frameRateNum = std::max(1, encode.outputFrameRate.numerator);
    const int32_t frameRateDen = std::max(1, encode.outputFrameRate.denominator);
    const int32_t wholeFps = std::max(1, frameRateNum / frameRateDen);
    const int32_t gop = std::max(1, encode.keyframeIntervalSec * wholeFps);

    // -thread_queue_size: the default (8 packets) is sized for on-demand file inputs, not
    // continuous live pipes fed by separate threads — too small here risks ffmpeg's input
    // queue filling and packets being dropped under any momentary scheduling hiccup.
    //
    // NOTE: -use_wallclock_as_timestamps was tried here (2026-09-29) to fix an A/V-sync
    // report and reverted the same day — it corrupted audio entirely. Reproduced in
    // isolation: two independently-scheduled real threads feeding separate raw inputs
    // produce wallclock timestamps that are NOT cleanly monotonic once interleaved,
    // which made ffmpeg's AAC encoder queue see input "backward in time" repeatedly
    // (`Non-monotonic DTS`, `Queue input is backward in time`, large `drop=` counts) —
    // exactly consistent with the resulting "no audio at all" regression. Do not re-add
    // this flag; if A/V sync is revisited, look elsewhere (e.g. actual pipe-connect
    // startup skew between the two inputs, or explicit -itsoffset on whichever stream is
    // consistently ahead) rather than wallclock timestamping raw threaded pipe inputs.
    std::wstringstream cmd;
    cmd << L"\"" << FindFfmpegExePath(encode.ffmpegExeName) << L"\""
        << L" -hide_banner -loglevel warning -y"
        << L" -thread_queue_size 1024"
        << L" -f rawvideo -pix_fmt bgra -s " << sourceWidth << L"x" << sourceHeight
        << L" -r " << frameRateNum << L"/" << frameRateDen
        << L" -i \"" << pipeName << L"\"";

    if (audio.enabled) {
        cmd << L" -thread_queue_size 1024"
            << L" -f " << (audio.isFloat ? L"f32le" : L"s16le")
            << L" -ar " << audio.sampleRate
            << L" -ac " << audio.channels
            << L" -i \"" << audio.pipeName << L"\"";
    }

    const std::wstring encoderImpl = encode.encoderImpl;
    cmd << L" -c:v " << encoderImpl;

    if (encoderImpl == L"libx264" && encode.x264Preset[0] != L'\0') {
        cmd << L" -preset " << encode.x264Preset;
    }

    // CcRateControl (the UI's CBR/VBR picker) reached this struct but was never actually
    // read here — `-b:v`/`-maxrate`/`-bufsize` alone don't force constant bitrate on any
    // encoder; without an explicit rate-control flag, NVENC in particular defaults to a
    // content-adaptive mode that can (and on a real DeckLink test, did: target 6000kbps,
    // actual ~2300-2700kbps) land well under the requested bitrate for easy-to-encode
    // content even with CBR selected in the UI. NVENC's `-rc`/libx264's `nal-hrd=cbr` are
    // the two encoder families this app actually ships (see MainViewModel.
    // AutoEncoderPreference); QSV/AMF aren't covered yet — left alone rather than guessing
    // an unverified flag for hardware this project hasn't tested against.
    const bool wantsCbr = encode.rateControl == CcRateControl::Cbr;
    const bool isNvenc = encoderImpl.find(L"nvenc") != std::wstring::npos;
    if (isNvenc) {
        cmd << L" -rc " << (wantsCbr ? L"cbr" : L"vbr");
    } else if (encoderImpl == L"libx264" && wantsCbr) {
        // Requires -maxrate/-bufsize (already always emitted below) to be set — x264's
        // CBR HRD mode paces output against them rather than just -b:v.
        cmd << L" -x264-params nal-hrd=cbr";
    }

    // Confirmed real bottleneck (2026-10-07, live DeckLink 1080p59.94 test on the
    // broadcast-account test machine): ffmpeg converts our raw BGRA frames to YUV420p via
    // libswscale on a single CPU core before handing them to the encoder, and that cost
    // is per-pixel, not bitrate-dependent (lowering -b:v from 6000k to 2000k didn't change
    // the achieved fps at all, stuck ~57fps against a 59.94 target; Task Manager confirmed
    // one core pegged near 100% while NVENC's own encode engine sat idle, and the same
    // machine streams this exact signal fine via vMix, ruling out a real hardware
    // ceiling). video's PTS timeline falls behind real elapsed time as a result while
    // audio (driven by actual sample count) doesn't — the likely proximate cause of a
    // multi-second A/V drift that persisted even after the OutputFrameRateNumerator/
    // Denominator fix in MainViewModel.BuildPresetEncode.
    // A GPU-side fix was tried and reverted the same day: `-vf
    // hwupload_cuda,scale_cuda=...format=nv12` (scale_cuda/hwupload_cuda are confirmed
    // present in this exact ffmpeg build via `-filters`) failed outright —
    // "Unsupported conversion: bgra -> semiplanar8" — scale_cuda's CUDA kernels only
    // reformat/resize within the YUV family (e.g. post hardware-decode nv12 cleanup);
    // they do not do RGB->YUV color-space conversion at all, so there is no GPU-filter
    // shortcut for a raw BGRA source on this ffmpeg build. Left at plain `-pix_fmt
    // yuv420p` (the CPU path) for every encoder, NVENC included, until a real fix lands —
    // see the project memory entry dated 2026-10-07 for the next avenue being
    // considered (converting straight from DeckLink's native UYVY to NV12 in our own
    // capture code, skipping the BGRA detour and ffmpeg's conversion entirely, rather
    // than guessing at more unverified ffmpeg flags against hardware we can't test here).
    cmd << L" -pix_fmt yuv420p";

    cmd << L" -b:v " << encode.bitrateKbps << L"k";
    if (encode.maxBitrateKbps > 0) {
        cmd << L" -maxrate " << encode.maxBitrateKbps << L"k";
    }
    if (encode.bufferSizeKbps > 0) {
        cmd << L" -bufsize " << encode.bufferSizeKbps << L"k";
    }
    cmd << L" -g " << gop << L" -keyint_min " << gop;

    if (encode.outputWidth > 0 && encode.outputHeight > 0) {
        cmd << L" -s " << encode.outputWidth << L"x" << encode.outputHeight;
    }

    if (audio.enabled) {
        // aresample=async=1: the video pump (paced by our own steady_clock loop) and
        // WASAPI audio capture (paced by its own real hardware clock) are two
        // independent clocks with no shared reference — even a tiny relative rate
        // difference between them (a few hundred ppm is normal for two unrelated
        // clocks) accumulates into audible A/V drift over a long-running stream
        // (confirmed: fine for ~60s, then noticeably desynced). async resampling
        // corrects this at the sample level — smoothly stretching/compressing audio to
        // track the video timeline — which is the standard fix for exactly this failure
        // mode. Deliberately NOT using -use_wallclock_as_timestamps for this (tried and
        // reverted the same day it was added: timestamping raw packets from two
        // independently-scheduled threads produced non-monotonic DTS and broke audio
        // entirely — see the comment above this function's ffmpeg path resolution).
        // first_pts=0 also normalizes the very start so a startup pipe-connect skew
        // between video and audio doesn't bake in an initial offset on top of the drift.
        //
        // Force the output rate: AAC only accepts a fixed set of sample rates, and the
        // WASAPI-negotiated input rate (audio.sampleRate) isn't guaranteed to be one of
        // them — ffmpeg resamples automatically when input/output -ar differ.
        cmd << L" -af aresample=async=1:first_pts=0"
            << L" -c:a aac -b:a " << encode.audioBitrateKbps << L"k -ar 48000";
    } else {
        cmd << L" -an";
    }

    cmd << L" -f mpegts \"" << BuildSrtUrl(srt) << L"\""
        << L" -progress pipe:1";

    return cmd.str();
}

bool FfmpegProcessController::Start(
    const CcEncodeSettings& encode,
    const CcSrtSettings& srt,
    const std::wstring& pipeName,
    int32_t sourceWidth,
    int32_t sourceHeight,
    const AudioPipeConfig& audio) {
    if (m_running.load()) {
        return false;
    }

    // If the previous ffmpeg process exited on its own (crash, SRT connect
    // failure/timeout, ...) without Stop() ever being called, m_progressThread finished
    // running but was never join()ed — std::thread stays joinable() until join()/detach()
    // regardless of whether its function has already returned. Reassigning it below via
    // `m_progressThread = std::thread(...)` while it's still joinable is undefined
    // behavior that calls std::terminate() per the standard, which is exactly the
    // "Debug Error / abort()" crash seen after a broken stream was restarted without an
    // intervening Stop(). Join it first so the reassignment is always safe.
    if (m_progressThread.joinable()) {
        m_progressThread.join();
    }
    if (m_stderrThread.joinable()) {
        m_stderrThread.join();
    }

    SECURITY_ATTRIBUTES pipeSecurity{};
    pipeSecurity.nLength = sizeof(SECURITY_ATTRIBUTES);
    pipeSecurity.bInheritHandle = TRUE;

    HANDLE stdoutWrite = nullptr;
    if (!CreatePipe(&m_stdoutReadPipe, &stdoutWrite, &pipeSecurity, 0)) {
        return false;
    }
    SetHandleInformation(m_stdoutReadPipe, HANDLE_FLAG_INHERIT, 0);

    HANDLE stderrWrite = nullptr;
    if (!CreatePipe(&m_stderrReadPipe, &stderrWrite, &pipeSecurity, 0)) {
        CloseHandle(m_stdoutReadPipe);
        m_stdoutReadPipe = nullptr;
        CloseHandle(stdoutWrite);
        return false;
    }
    SetHandleInformation(m_stderrReadPipe, HANDLE_FLAG_INHERIT, 0);

    // stdin is a pipe (not NUL) so Stop() can ask ffmpeg to quit gracefully by writing
    // 'q' — its documented interactive-mode quit command — rather than only ever
    // force-killing it, which otherwise ends the SRT session abruptly (the receiving
    // side sees a mid-stream I/O error instead of a clean close).
    HANDLE stdinRead = nullptr;
    if (!CreatePipe(&stdinRead, &m_stdinWritePipe, &pipeSecurity, 0)) {
        CloseHandle(m_stdoutReadPipe);
        m_stdoutReadPipe = nullptr;
        CloseHandle(m_stderrReadPipe);
        m_stderrReadPipe = nullptr;
        CloseHandle(stderrWrite);
        return false;
    }
    SetHandleInformation(m_stdinWritePipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(STARTUPINFOW);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdInput = stdinRead;
    startupInfo.hStdOutput = stdoutWrite;
    startupInfo.hStdError = stderrWrite;

    std::wstring commandLine = BuildCommandLine(encode, srt, pipeName, sourceWidth, sourceHeight, audio);
    // Logged unconditionally (not just on failure): the alternative is reconstructing it
    // by hand from settings whenever ffmpeg rejects something, which is slow and has
    // already produced at least one wrong guess this session (a hand-built repro that
    // didn't actually match the real generated command and so didn't reproduce a real
    // argument-parsing bug). Passphrase/streamid could appear in here via the SRT URL —
    // this file is already local-machine-only diagnostic output in the same place
    // ffmpeg's own stderr is captured, not a new exposure.
    LogDiagnostic(L"FfmpegProcessController::Start: " + commandLine);
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    ZeroMemory(&m_processInfo, sizeof(m_processInfo));
    const BOOL created = CreateProcessW(
        nullptr,
        mutableCommandLine.data(),
        nullptr,
        nullptr,
        /*bInheritHandles=*/TRUE,
        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
        nullptr,
        nullptr,
        &startupInfo,
        &m_processInfo);

    // The child has its own handles to these now (or failed to start); the parent's
    // copies must be closed so ReadFile on m_stdoutReadPipe/m_stderrReadPipe unblocks on
    // child exit.
    CloseHandle(stdoutWrite);
    CloseHandle(stdinRead);
    CloseHandle(stderrWrite);

    if (!created) {
        CloseHandle(m_stdoutReadPipe);
        m_stdoutReadPipe = nullptr;
        CloseHandle(m_stdinWritePipe);
        m_stdinWritePipe = nullptr;
        CloseHandle(m_stderrReadPipe);
        m_stderrReadPipe = nullptr;
        return false;
    }

    m_stopRequested.store(false);
    m_running.store(true);
    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats = CcStreamStats{};
        m_stats.connectionState = CcConnectionState::Connecting;
    }
    m_progressThread = std::thread(&FfmpegProcessController::ProgressThreadMain, this);
    m_stderrThread = std::thread(&FfmpegProcessController::StderrThreadMain, this);
    return true;
}

void FfmpegProcessController::StderrThreadMain() {
    std::string lineBuffer;
    char readBuffer[4096];

    for (;;) {
        DWORD bytesRead = 0;
        const BOOL ok = ReadFile(m_stderrReadPipe, readBuffer, sizeof(readBuffer), &bytesRead, nullptr);
        if (!ok || bytesRead == 0) {
            break; // pipe closed: ffmpeg exited
        }
        lineBuffer.append(readBuffer, bytesRead);

        size_t newlinePos;
        while ((newlinePos = lineBuffer.find('\n')) != std::string::npos) {
            std::string line = lineBuffer.substr(0, newlinePos);
            lineBuffer.erase(0, newlinePos + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!line.empty()) {
                std::wstring wLine(line.begin(), line.end()); // ffmpeg's own log text is ASCII/UTF-8-as-bytes here
                LogDiagnostic(L"ffmpeg: " + wLine);
            }
        }
    }
    // Flush any final partial line ffmpeg wrote without a trailing newline before exiting.
    if (!lineBuffer.empty()) {
        std::wstring wLine(lineBuffer.begin(), lineBuffer.end());
        LogDiagnostic(L"ffmpeg: " + wLine);
    }
}

void FfmpegProcessController::ProgressThreadMain() {
    LogDiagnostic(L"ProgressThreadMain: started");
    std::string lineBuffer;
    char readBuffer[4096];
    // Real-time encode performance (fps vs. target, growing bitrate/queue backlog) was
    // previously invisible outside the live UI — logged periodically now so a
    // fell-behind-real-time encoder (the suspected cause of a growing A/V delay that
    // survived the audio-queue fix — i.e. NOT lost upstream, just perpetually late) is
    // diagnosable after the fact instead of needing someone watching the stats bar live.
    auto lastProgressLogTime = std::chrono::steady_clock::now() - std::chrono::seconds(10);

    for (;;) {
        DWORD bytesRead = 0;
        const BOOL ok = ReadFile(m_stdoutReadPipe, readBuffer, sizeof(readBuffer), &bytesRead, nullptr);
        if (!ok || bytesRead == 0) {
            LogDiagnostic(L"ProgressThreadMain: ReadFile ended (ok=" + std::wstring(ok ? L"yes" : L"no") +
                          L" bytesRead=" + std::to_wstring(bytesRead) + L")");
            break; // pipe closed: ffmpeg exited
        }
        lineBuffer.append(readBuffer, bytesRead);

        size_t newlinePos;
        while ((newlinePos = lineBuffer.find('\n')) != std::string::npos) {
            std::string line = lineBuffer.substr(0, newlinePos);
            lineBuffer.erase(0, newlinePos + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            std::wstring wLine(line.begin(), line.end()); // progress keys/values are ASCII

            CcStreamStats snapshot{};
            {
                std::lock_guard<std::mutex> lock(m_statsMutex);
                ApplyProgressLine(wLine, m_stats);
                snapshot = m_stats;
            }

            if (wLine.rfind(L"progress=", 0) == 0) {
                const auto now = std::chrono::steady_clock::now();
                if (now - lastProgressLogTime >= std::chrono::seconds(5)) {
                    lastProgressLogTime = now;
                    LogDiagnostic(L"progress: fps=" + std::to_wstring(snapshot.fps) +
                                  L" bitrateKbps=" + std::to_wstring(snapshot.bitrateKbps) +
                                  L" framesEncoded=" + std::to_wstring(snapshot.framesEncoded) +
                                  L" droppedFrames=" + std::to_wstring(snapshot.droppedFrames));
                }
            }
        }
    }

    LogDiagnostic(L"ProgressThreadMain: waiting for process exit...");
    WaitForSingleObject(m_processInfo.hProcess, INFINITE);
    LogDiagnostic(L"ProgressThreadMain: process exited");

    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        if (!m_stopRequested.load()) {
            // ffmpeg exited on its own (crash, SRT connection refused, etc.), not
            // because we asked it to stop.
            m_stats.connectionState = CcConnectionState::Broken;
        }
    }
    m_running.store(false);
    LogDiagnostic(L"ProgressThreadMain: exiting");
}

void FfmpegProcessController::Stop() {
    LogDiagnostic(L"FfmpegProcessController::Stop() called, running=" +
                  std::wstring(m_running.load() ? L"yes" : L"no"));
    if (!m_running.load() && m_processInfo.hProcess == nullptr) {
        LogDiagnostic(L"  already stopped, returning");
        return;
    }

    m_stopRequested.store(true);

    if (m_processInfo.hProcess) {
        // Best-effort: ffmpeg's interactive keyboard-command handling on Windows reads
        // via console APIs (PeekConsoleInput/ReadConsoleInput), which don't work on a
        // redirected pipe — empirically confirmed this never gets picked up within any
        // reasonable wait when stdin is piped (as it must be here). Left in as a no-cost
        // attempt in case that ever changes, but Windows gives us no real path to a
        // clean ffmpeg shutdown short of allocating it a real console just for this, so
        // the wait is kept short rather than paying a multi-second penalty for a command
        // that structurally can't be received. The SRT receiver sees an abrupt
        // disconnect rather than a clean close as a result — acceptable for this app's
        // use case (a dropped connection is routine for any SRT receiver to handle).
        LogDiagnostic(L"  requesting graceful quit ('q' on stdin, best-effort)...");
        if (m_stdinWritePipe) {
            const char quitCommand[] = "q\n";
            DWORD written = 0;
            WriteFile(m_stdinWritePipe, quitCommand, sizeof(quitCommand) - 1, &written, nullptr);
        }
        if (WaitForSingleObject(m_processInfo.hProcess, 300) != WAIT_OBJECT_0) {
            LogDiagnostic(L"  graceful quit didn't land, calling TerminateProcess...");
            TerminateProcess(m_processInfo.hProcess, 0);
        }
        LogDiagnostic(L"  ffmpeg process ended");
    }

    LogDiagnostic(L"  joining progress thread...");
    if (m_progressThread.joinable()) {
        m_progressThread.join();
    }
    LogDiagnostic(L"  progress thread joined");

    if (m_stderrThread.joinable()) {
        m_stderrThread.join();
    }

    if (m_stdoutReadPipe) {
        CloseHandle(m_stdoutReadPipe);
        m_stdoutReadPipe = nullptr;
    }
    if (m_stderrReadPipe) {
        CloseHandle(m_stderrReadPipe);
        m_stderrReadPipe = nullptr;
    }
    if (m_stdinWritePipe) {
        CloseHandle(m_stdinWritePipe);
        m_stdinWritePipe = nullptr;
    }
    if (m_processInfo.hProcess) {
        CloseHandle(m_processInfo.hProcess);
        CloseHandle(m_processInfo.hThread);
        ZeroMemory(&m_processInfo, sizeof(m_processInfo));
    }

    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.connectionState = CcConnectionState::Stopped;
    }
    m_running.store(false);
    LogDiagnostic(L"FfmpegProcessController::Stop() done");
}

bool FfmpegProcessController::IsRunning() const {
    return m_running.load();
}

CcStreamStats FfmpegProcessController::GetStats() const {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_stats;
}

} // namespace capturecore
