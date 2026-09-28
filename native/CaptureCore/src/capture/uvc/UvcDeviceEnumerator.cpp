#include "UvcDeviceEnumerator.h"
#include "UvcCaptureSource.h"

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
    ActivateArray activates;
    if (!EnumerateVideoCaptureActivates(activates)) {
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

        ComPtr<IMFMediaSource> mediaSource;
        if (FAILED(activates.items[i]->ActivateObject(IID_PPV_ARGS(&mediaSource)))) {
            return nullptr;
        }

        try {
            return std::make_unique<UvcCaptureSource>(std::move(mediaSource), format);
        } catch (const std::exception&) {
            return nullptr;
        }
    }

    return nullptr;
}

} // namespace capturecore
