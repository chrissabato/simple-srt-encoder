#include "capturecore/capturecore_api.h"

#include <string>

namespace {

std::wstring BuildVersionString() {
    std::wstring backends = L"UVC";
#if defined(CAPTURECORE_ENABLE_DECKLINK)
    backends += L"+DeckLink";
#endif
#if defined(CAPTURECORE_ENABLE_NDI)
    backends += L"+NDI";
#endif
    return L"CaptureCore 0.1.0-dev (" + backends + L")";
}

} // namespace

int32_t CaptureCore_GetVersionString(wchar_t* buffer, int32_t bufferCapacity) {
    const std::wstring version = BuildVersionString();
    const int32_t requiredCapacity = static_cast<int32_t>(version.size()) + 1;

    if (buffer == nullptr || bufferCapacity < requiredCapacity) {
        return requiredCapacity;
    }

    version.copy(buffer, version.size());
    buffer[version.size()] = L'\0';
    return requiredCapacity;
}
