#include "FfmpegProcessController.h"
#include "SrtUrlBuilder.h"

#include <algorithm>
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

std::wstring FfmpegProcessController::FindFfmpegExePath() {
    return GetExecutableDirectory() + L"\\ffmpeg\\ffmpeg.exe";
}

std::wstring FfmpegProcessController::BuildCommandLine(
    const CcEncodeSettings& encode,
    const CcSrtSettings& srt,
    const std::wstring& pipeName,
    int32_t sourceWidth,
    int32_t sourceHeight) const {
    const int32_t frameRateNum = std::max(1, encode.outputFrameRate.numerator);
    const int32_t frameRateDen = std::max(1, encode.outputFrameRate.denominator);
    const int32_t wholeFps = std::max(1, frameRateNum / frameRateDen);
    const int32_t gop = std::max(1, encode.keyframeIntervalSec * wholeFps);

    std::wstringstream cmd;
    cmd << L"\"" << FindFfmpegExePath() << L"\""
        << L" -hide_banner -loglevel warning -y"
        << L" -f rawvideo -pix_fmt bgra -s " << sourceWidth << L"x" << sourceHeight
        << L" -r " << frameRateNum << L"/" << frameRateDen
        << L" -i \"" << pipeName << L"\""
        << L" -an" // audio capture is a Phase 4 item; not wired up yet
        << L" -c:v " << encode.encoderImpl;

    if (std::wstring(encode.encoderImpl) == L"libx264" && encode.x264Preset[0] != L'\0') {
        cmd << L" -preset " << encode.x264Preset;
    }

    cmd << L" -pix_fmt yuv420p"
        << L" -b:v " << encode.bitrateKbps << L"k";
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

    cmd << L" -f mpegts \"" << BuildSrtUrl(srt) << L"\""
        << L" -progress pipe:1";

    return cmd.str();
}

bool FfmpegProcessController::Start(
    const CcEncodeSettings& encode,
    const CcSrtSettings& srt,
    const std::wstring& pipeName,
    int32_t sourceWidth,
    int32_t sourceHeight) {
    if (m_running.load()) {
        return false;
    }

    SECURITY_ATTRIBUTES pipeSecurity{};
    pipeSecurity.nLength = sizeof(SECURITY_ATTRIBUTES);
    pipeSecurity.bInheritHandle = TRUE;

    HANDLE stdoutWrite = nullptr;
    if (!CreatePipe(&m_stdoutReadPipe, &stdoutWrite, &pipeSecurity, 0)) {
        return false;
    }
    SetHandleInformation(m_stdoutReadPipe, HANDLE_FLAG_INHERIT, 0);

    HANDLE nulHandle = CreateFileW(
        L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &pipeSecurity, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(STARTUPINFOW);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdInput = nulHandle;
    startupInfo.hStdOutput = stdoutWrite;
    startupInfo.hStdError = nulHandle;

    std::wstring commandLine = BuildCommandLine(encode, srt, pipeName, sourceWidth, sourceHeight);
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
    // copies must be closed so ReadFile on m_stdoutReadPipe unblocks on child exit.
    CloseHandle(stdoutWrite);
    if (nulHandle) {
        CloseHandle(nulHandle);
    }

    if (!created) {
        CloseHandle(m_stdoutReadPipe);
        m_stdoutReadPipe = nullptr;
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
    return true;
}

void FfmpegProcessController::ProgressThreadMain() {
    std::string lineBuffer;
    char readBuffer[4096];

    for (;;) {
        DWORD bytesRead = 0;
        const BOOL ok = ReadFile(m_stdoutReadPipe, readBuffer, sizeof(readBuffer), &bytesRead, nullptr);
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
            std::wstring wLine(line.begin(), line.end()); // progress keys/values are ASCII
            std::lock_guard<std::mutex> lock(m_statsMutex);
            ApplyProgressLine(wLine, m_stats);
        }
    }

    WaitForSingleObject(m_processInfo.hProcess, INFINITE);

    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        if (!m_stopRequested.load()) {
            // ffmpeg exited on its own (crash, SRT connection refused, etc.), not
            // because we asked it to stop.
            m_stats.connectionState = CcConnectionState::Broken;
        }
    }
    m_running.store(false);
}

void FfmpegProcessController::Stop() {
    if (!m_running.load() && m_processInfo.hProcess == nullptr) {
        return;
    }

    m_stopRequested.store(true);

    if (m_processInfo.hProcess) {
        GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, m_processInfo.dwProcessId);
        if (WaitForSingleObject(m_processInfo.hProcess, 2000) != WAIT_OBJECT_0) {
            TerminateProcess(m_processInfo.hProcess, 0);
        }
    }

    if (m_progressThread.joinable()) {
        m_progressThread.join();
    }

    if (m_stdoutReadPipe) {
        CloseHandle(m_stdoutReadPipe);
        m_stdoutReadPipe = nullptr;
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
}

bool FfmpegProcessController::IsRunning() const {
    return m_running.load();
}

CcStreamStats FfmpegProcessController::GetStats() const {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_stats;
}

} // namespace capturecore
