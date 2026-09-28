#include "SrtUrlBuilder.h"

#include <sstream>
#include <iomanip>

namespace capturecore {

namespace {

// Minimal percent-encoding for URL query values (streamid/passphrase may contain
// characters like '&', '=', ' ' that would otherwise break the query string).
std::wstring UrlEncode(const std::wstring& value) {
    std::wstringstream out;
    for (wchar_t ch : value) {
        const bool isUnreserved =
            (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z') ||
            (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_' || ch == L'.' || ch == L'~';
        if (isUnreserved) {
            out << ch;
        } else {
            out << L'%' << std::uppercase << std::hex << std::setw(2) << std::setfill(L'0')
                << static_cast<unsigned int>(static_cast<unsigned char>(ch));
        }
    }
    return out.str();
}

const wchar_t* ModeToString(CcSrtMode mode) {
    switch (mode) {
        case CcSrtMode::Caller: return L"caller";
        case CcSrtMode::Listener: return L"listener";
        case CcSrtMode::Rendezvous: return L"rendezvous";
    }
    return L"caller";
}

} // namespace

std::wstring BuildSrtUrl(const CcSrtSettings& srt) {
    std::wstringstream url;
    url << L"srt://" << srt.host << L":" << srt.port
        << L"?mode=" << ModeToString(srt.mode)
        << L"&latency=" << srt.latencyMs;

    if (srt.passphrase[0] != L'\0') {
        url << L"&passphrase=" << UrlEncode(srt.passphrase);
        if (srt.pbkeylen == 16 || srt.pbkeylen == 24 || srt.pbkeylen == 32) {
            url << L"&pbkeylen=" << srt.pbkeylen;
        }
    }
    if (srt.streamId[0] != L'\0') {
        url << L"&streamid=" << UrlEncode(srt.streamId);
    }

    return url.str();
}

} // namespace capturecore
