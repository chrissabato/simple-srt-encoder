#include "UvcDeviceEnumerator.h"
#include "UvcCaptureSource.h"
#include "../../Diagnostics.h"

#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace capturecore {

namespace {

// RAII helper for the CoTaskMemAlloc'd array MFEnumDeviceSources hands back.
struct ActivateArray {
    IMFActivate** items = nullptr;
    UINT32 count = 0;

    ~ActivateArray() {
        for (UINT32 i = 0; i < count; ++i) {
            if (items[i]) {
                items[i]->Release();
            }
        }
        CoTaskMemFree(items);
    }
};

bool EnumerateVideoCaptureActivates(ActivateArray& out) {
    ComPtr<IMFAttributes> attributes;
    if (FAILED(MFCreateAttributes(&attributes, 1))) {
        return false;
    }
    if (FAILED(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) {
        return false;
    }
    return SUCCEEDED(MFEnumDeviceSources(attributes.Get(), &out.items, &out.count));
}

void CopyWString(wchar_t* dest, size_t destCapacity, const wchar_t* src) {
    if (!src) {
        dest[0] = L'\0';
        return;
    }
    wcsncpy_s(dest, destCapacity, src, _TRUNCATE);
}

} // namespace

std::vector<CcDeviceInfo> UvcDeviceEnumerator::Enumerate() {
    std::vector<CcDeviceInfo> devices;

    ActivateArray activates;
    if (!EnumerateVideoCaptureActivates(activates)) {
        return devices;
    }

    for (UINT32 i = 0; i < activates.count; ++i) {
        wchar_t* friendlyName = nullptr;
        UINT32 friendlyNameLength = 0;
        activates.items[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &friendlyName, &friendlyNameLength);

        wchar_t* symbolicLink = nullptr;
        UINT32 symbolicLinkLength = 0;
        activates.items[i]->GetAllocatedString(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &symbolicLink, &symbolicLinkLength);

        if (symbolicLink) {
            CcDeviceInfo info{};
            info.backend = CcBackendType::Uvc;
            CopyWString(info.id.value, CC_MAX_STRING, symbolicLink);
            CopyWString(info.displayName, CC_MAX_STRING, friendlyName);
            devices.push_back(info);
        }

        CoTaskMemFree(friendlyName);
        CoTaskMemFree(symbolicLink);
    }

    return devices;
}

std::unique_ptr<ICaptureSource> UvcDeviceEnumerator::Open(const CcDeviceId& id, const CcCaptureFormat& format) {
    LogDiagnostic(L"UvcDeviceEnumerator::Open requested id=" + std::wstring(id.value));

    ActivateArray activates;
    if (!EnumerateVideoCaptureActivates(activates)) {
        LogDiagnostic(L"  EnumerateVideoCaptureActivates failed");
        return nullptr;
    }

    for (UINT32 i = 0; i < activates.count; ++i) {
        wchar_t* symbolicLink = nullptr;
        UINT32 symbolicLinkLength = 0;
        activates.items[i]->GetAllocatedString(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &symbolicLink, &symbolicLinkLength);

        const bool matches = symbolicLink && wcscmp(symbolicLink, id.value) == 0;
        CoTaskMemFree(symbolicLink);
        if (!matches) {
            continue;
        }

        LogDiagnostic(L"  matched device at index " + std::to_wstring(i) + L", activating...");

        ComPtr<IMFMediaSource> mediaSource;
        const HRESULT activateHr = activates.items[i]->ActivateObject(IID_PPV_ARGS(&mediaSource));
        if (FAILED(activateHr)) {
            LogDiagnostic(L"  ActivateObject FAILED hr=0x" + std::to_wstring(static_cast<unsigned long>(activateHr)));
            return nullptr;
        }

        try {
            LogDiagnostic(L"  ActivateObject OK, constructing UvcCaptureSource...");
            auto source = std::make_unique<UvcCaptureSource>(std::move(mediaSource), format);
            LogDiagnostic(L"  UvcCaptureSource constructed OK");
            return source;
        } catch (const std::exception& ex) {
            std::string what = ex.what();
            LogDiagnostic(L"  UvcCaptureSource threw: " + std::wstring(what.begin(), what.end()));
            return nullptr;
        }
    }

    LogDiagnostic(L"  no matching device found among " + std::to_wstring(activates.count) + L" enumerated");
    return nullptr;
}

} // namespace capturecore
