#include "Diagnostics.h"

#include <fstream>
#include <mutex>
#include <windows.h>

namespace capturecore {

namespace {
std::mutex g_logMutex;
}

void LogDiagnostic(const std::wstring& message) {
    std::lock_guard<std::mutex> lock(g_logMutex);

    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    const std::wstring logPath = std::wstring(tempPath) + L"CaptureCore.log";

    std::wofstream file(logPath, std::ios::app);
    if (!file) {
        return;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t timestamp[32];
    swprintf_s(timestamp, L"%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    file << L"[" << timestamp << L"] " << message << std::endl;
}

} // namespace capturecore
