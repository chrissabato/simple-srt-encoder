#include "capturecore/capturecore_api.h"
#include "capture/CaptureManager.h"

#include <string>

using capturecore::CaptureManager;

namespace {

CaptureManager* ToManager(CaptureCoreHandle handle) {
    return static_cast<CaptureManager*>(handle);
}

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

CaptureCoreHandle CaptureCore_Create() {
    return new CaptureManager();
}

void CaptureCore_Destroy(CaptureCoreHandle handle) {
    delete ToManager(handle);
}

int32_t CaptureCore_IsBackendAvailable(CcBackendType backend) {
    switch (backend) {
        case CcBackendType::Uvc:
            return 1;
        case CcBackendType::DeckLink:
#if defined(CAPTURECORE_ENABLE_DECKLINK)
            return 1;
#else
            return 0;
#endif
        case CcBackendType::Ndi:
#if defined(CAPTURECORE_ENABLE_NDI)
            return 1;
#else
            return 0;
#endif
    }
    return 0;
}

int32_t CaptureCore_EnumerateDevices(CaptureCoreHandle handle, CcDeviceInfo* outArray, int32_t maxCount) {
    return ToManager(handle)->EnumerateDevices(outArray, maxCount);
}

int32_t CaptureCore_EnumerateAudioDevices(CaptureCoreHandle handle, CcAudioDeviceInfo* outArray, int32_t maxCount) {
    return ToManager(handle)->EnumerateAudioDevices(outArray, maxCount);
}

int32_t CaptureCore_StartAudioMonitor(CaptureCoreHandle handle, const CcDeviceId* id) {
    return ToManager(handle)->StartAudioMonitor(*id) ? 1 : 0;
}

void CaptureCore_StopAudioMonitor(CaptureCoreHandle handle) {
    ToManager(handle)->StopAudioMonitor();
}

int32_t CaptureCore_GetLoudness(CaptureCoreHandle handle, CcLoudness* outLoudness) {
    *outLoudness = ToManager(handle)->GetLoudness();
    return 1;
}

void CaptureCore_ResetLoudness(CaptureCoreHandle handle) {
    ToManager(handle)->ResetLoudness();
}

int32_t CaptureCore_OpenSource(CaptureCoreHandle handle, const CcDeviceId* id, const CcCaptureFormat* format) {
    return ToManager(handle)->OpenSource(*id, *format) ? 1 : 0;
}

void CaptureCore_CloseSource(CaptureCoreHandle handle) {
    ToManager(handle)->CloseSource();
}

int32_t CaptureCore_GetOpenSourceSize(CaptureCoreHandle handle, int32_t* outWidth, int32_t* outHeight) {
    return ToManager(handle)->GetOpenSourceSize(*outWidth, *outHeight) ? 1 : 0;
}

int32_t CaptureCore_TryGetLatestFrame(CaptureCoreHandle handle, CcFrameBuffer* outFrame) {
    return ToManager(handle)->TryGetLatestFrame(*outFrame) ? 1 : 0;
}

int32_t CaptureCore_StartStream(CaptureCoreHandle handle, const CcEncodeSettings* encode, const CcSrtSettings* srt) {
    return ToManager(handle)->StartStream(*encode, *srt) ? 1 : 0;
}

void CaptureCore_StopStream(CaptureCoreHandle handle) {
    ToManager(handle)->StopStream();
}

int32_t CaptureCore_IsStreaming(CaptureCoreHandle handle) {
    return ToManager(handle)->IsStreaming() ? 1 : 0;
}

int32_t CaptureCore_GetStreamStats(CaptureCoreHandle handle, CcStreamStats* outStats) {
    *outStats = ToManager(handle)->GetStreamStats();
    return 1;
}

int32_t CaptureCore_ProbeEncoder(CaptureCoreHandle handle, const wchar_t* encoderName, const wchar_t* ffmpegExeName) {
    return ToManager(handle)->ProbeEncoder(encoderName, ffmpegExeName ? ffmpegExeName : L"") ? 1 : 0;
}
