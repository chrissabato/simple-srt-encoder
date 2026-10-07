#include "Diagnostics.h"

#include <fstream>
#include <mutex>
#include <string>
#include <windows.h>

namespace capturecore {

namespace {
std::mutex g_logMutex;

// std::wofstream with no locale imbued (the default) uses the classic "C" locale's
// codecvt facet to convert wchar_t -> narrow bytes, which can only represent ASCII
// (0-127) — writing any wide character outside that range (e.g. an em dash, '—',
// U+2014, used liberally in several diagnostic messages added this session) puts the
// stream into a failed state partway through that single `<<` chain, silently dropping
// everything after it INCLUDING the trailing std::endl. The visible symptom: a log line
// containing any non-ASCII character gets truncated mid-message with no newline, so the
// next LogDiagnostic() call's text runs on right after it, looking like one garbled
// line — and the truncated part is often exactly the diagnostic payload (e.g. an
// HRESULT) a message was written to capture. Converting to UTF-8 explicitly and writing
// as plain bytes (not through a wchar_t-aware stream at all) sidesteps the whole
// locale/codecvt pitfall.
std::string ToUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int sizeNeeded =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(sizeNeeded), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), sizeNeeded, nullptr, nullptr);
    return utf8;
}
} // namespace

void LogDiagnostic(const std::wstring& message) {
    std::lock_guard<std::mutex> lock(g_logMutex);

    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    const std::wstring logPath = std::wstring(tempPath) + L"CaptureCore.log";

    std::ofstream file(logPath, std::ios::app | std::ios::binary);
    if (!file) {
        return;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    char timestamp[32];
    sprintf_s(timestamp, "%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    file << "[" << timestamp << "] " << ToUtf8(message) << "\n";
}

} // namespace capturecore
