#include "SrtUrlBuilder.h"

#include <sstream>
#include <iomanip>
#include <windows.h>

namespace capturecore {

namespace {

// Minimal percent-encoding for URL query values (streamid/passphrase may contain
// characters like '&', '=', ' ' that would otherwise break the query string).
//
// Percent-encodes UTF-8 bytes, not raw UTF-16 code units: casting a wchar_t directly to
// unsigned char (the previous approach) silently truncated every code point above U+00FF
// to its low byte, producing a mangled result instead of a valid percent-encoded
// sequence for any non-ASCII streamid/passphrase.
std::wstring UrlEncode(const std::wstring& value) {
    if (value.empty()) {
        return L"";
    }
    const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(utf8Length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, utf8.data(), utf8Length, nullptr, nullptr);

    std::wstringstream out;
    for (unsigned char ch : utf8) {
        if (ch == '\0') {
            break; // WideCharToMultiByte's -1 length includes the terminator
        }
        const bool isUnreserved =
            (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~';
        if (isUnreserved) {
            out << static_cast<wchar_t>(ch);
        } else {
            out << L'%' << std::uppercase << std::hex << std::setw(2) << std::setfill(L'0')
                << static_cast<unsigned int>(ch);
        }
    }
    return out.str();
}

// Wraps a literal IPv6 host (containing ':') in brackets per RFC 3986 — srt.host is
// taken as-is otherwise. Without this, "::1" produced "srt://::1:9000", which is
// ambiguous between host and port and fails to parse as the intended address.
std::wstring FormatHost(const std::wstring& host) {
    if (host.find(L':') != std::wstring::npos && !host.empty() && host.front() != L'[') {
        return L"[" + host + L"]";
    }
    return host;
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
    url << L"srt://" << FormatHost(srt.host) << L":" << srt.port
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
