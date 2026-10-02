#include "DirectShowDeviceEnumerator.h"
#include "DirectShowCaptureSource.h"
#include "DirectShowGuids.h"
#include "../../Diagnostics.h"

#include <wrl/client.h>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace capturecore {

namespace {

void CopyWString(wchar_t* dest, size_t destCapacity, const wchar_t* src) {
    if (!src) {
        dest[0] = L'\0';
        return;
    }
    wcsncpy_s(dest, destCapacity, src, _TRUNCATE);
}

// True for filters registered directly as software (not PnP hardware) capture
// sources — see the class comment in the header for why only these are enumerated.
// The raw moniker display name uses colons ("@device:sw:{...}"); ffmpeg's dshow
// demuxer prints an underscore-escaped variant of this same string as its
// "Alternative name" (shell-safe for its own -i syntax), which is not what
// IMoniker::GetDisplayName actually returns — worth noting since that's a very easy
// mix-up when cross-checking against `ffmpeg -f dshow -list_devices`.
bool IsSoftwareFilterPath(const wchar_t* devicePath) {
    return devicePath && wcsncmp(devicePath, L"@device:sw:", 11) == 0;
}

std::wstring ReadFriendlyName(IMoniker* moniker) {
    ComPtr<IPropertyBag> propBag;
    if (FAILED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&propBag)))) {
        return L"";
    }
    VARIANT varName;
    VariantInit(&varName);
    std::wstring name;
    if (SUCCEEDED(propBag->Read(L"FriendlyName", &varName, nullptr)) && varName.vt == VT_BSTR && varName.bstrVal) {
        name = varName.bstrVal;
    }
    VariantClear(&varName);
    return name;
}

bool CreateVideoInputEnumerator(ComPtr<IEnumMoniker>& outEnumMoniker) {
    ComPtr<ICreateDevEnum> devEnum;
    if (FAILED(CoCreateInstance(
            CLSID_DShow_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devEnum)))) {
        return false;
    }
    // S_FALSE (not just a failed HRESULT) means the category exists but has zero
    // registered devices — outEnumMoniker is left null either way, which the caller
    // already treats as "nothing to enumerate".
    return devEnum->CreateClassEnumerator(CLSID_DShow_VideoInputDeviceCategory, &outEnumMoniker, 0) == S_OK;
}

} // namespace

std::vector<CcDeviceInfo> DirectShowDeviceEnumerator::Enumerate() {
    std::vector<CcDeviceInfo> devices;

    ComPtr<IEnumMoniker> enumMoniker;
    if (!CreateVideoInputEnumerator(enumMoniker) || !enumMoniker) {
        return devices;
    }

    ComPtr<IMoniker> moniker;
    while (enumMoniker->Next(1, &moniker, nullptr) == S_OK) {
        LPOLESTR displayName = nullptr;
        if (SUCCEEDED(moniker->GetDisplayName(nullptr, nullptr, &displayName)) && displayName) {
            if (IsSoftwareFilterPath(displayName)) {
                const std::wstring friendlyName = ReadFriendlyName(moniker.Get());

                CcDeviceInfo info{};
                info.backend = CcBackendType::DirectShow;
                CopyWString(info.id.value, CC_MAX_STRING, displayName);
                CopyWString(
                    info.displayName, CC_MAX_STRING, friendlyName.empty() ? displayName : friendlyName.c_str());
                devices.push_back(info);
            }
            CoTaskMemFree(displayName);
        }
        moniker.Reset();
    }

    return devices;
}

std::unique_ptr<ICaptureSource> DirectShowDeviceEnumerator::Open(const CcDeviceId& id, const CcCaptureFormat& format) {
    ComPtr<IEnumMoniker> enumMoniker;
    if (!CreateVideoInputEnumerator(enumMoniker) || !enumMoniker) {
        return nullptr;
    }

    ComPtr<IMoniker> moniker;
    while (enumMoniker->Next(1, &moniker, nullptr) == S_OK) {
        LPOLESTR displayName = nullptr;
        const bool matches = SUCCEEDED(moniker->GetDisplayName(nullptr, nullptr, &displayName)) && displayName &&
            wcscmp(displayName, id.value) == 0;
        if (displayName) {
            CoTaskMemFree(displayName);
        }
        if (!matches) {
            moniker.Reset();
            continue;
        }

        ComPtr<IBaseFilter> filter;
        if (FAILED(moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&filter)))) {
            LogDiagnostic(L"DirectShowDeviceEnumerator::Open: BindToObject failed");
            return nullptr;
        }

        try {
            return std::make_unique<DirectShowCaptureSource>(std::move(filter), format);
        } catch (const std::exception& ex) {
            std::string what = ex.what();
            LogDiagnostic(L"DirectShowDeviceEnumerator::Open: DirectShowCaptureSource threw: " +
                          std::wstring(what.begin(), what.end()));
            return nullptr;
        }
    }

    return nullptr;
}

} // namespace capturecore
