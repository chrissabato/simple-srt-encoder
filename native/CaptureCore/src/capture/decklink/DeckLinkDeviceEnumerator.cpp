#include "DeckLinkDeviceEnumerator.h"
#include "DeckLinkCaptureSource.h"
#include "../../Diagnostics.h"

#include <DeckLinkAPI.h>
#include <wrl/client.h>

#include <sstream>

using Microsoft::WRL::ComPtr;

namespace capturecore {

namespace {

// A DeckLink device has no symbolic-link-style identifier the way Media Foundation
// devices do; BMDDeckLinkPersistentID is the SDK's own documented stable identifier
// for a physical device across enumerations, so it's used as the opaque CcDeviceId.
std::wstring MakeDeviceId(int64_t persistentId) {
    std::wstringstream ss;
    ss << L"decklink:" << std::hex << persistentId;
    return ss.str();
}

bool TryGetInputAndPersistentId(IDeckLink* deckLink, ComPtr<IDeckLinkInput>& outInput, int64_t& outPersistentId) {
    ComPtr<IDeckLink> device(deckLink);

    if (FAILED(device.As(&outInput))) {
        return false; // output-only device or a sub-device with no input on this profile
    }

    ComPtr<IDeckLinkProfileAttributes> attributes;
    if (FAILED(device.As(&attributes))) {
        return false;
    }
    outPersistentId = 0;
    if (FAILED(attributes->GetInt(BMDDeckLinkPersistentID, &outPersistentId)) || outPersistentId == 0) {
        return false;
    }
    return true;
}

} // namespace

bool DeckLinkDeviceEnumerator::IsAvailable() const {
    ComPtr<IDeckLinkIterator> iterator;
    return SUCCEEDED(CoCreateInstance(
        CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL, IID_IDeckLinkIterator, &iterator));
}

std::vector<CcDeviceInfo> DeckLinkDeviceEnumerator::Enumerate() {
    std::vector<CcDeviceInfo> devices;

    ComPtr<IDeckLinkIterator> iterator;
    if (FAILED(CoCreateInstance(
            CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL, IID_IDeckLinkIterator, &iterator))) {
        LogDiagnostic(L"DeckLinkDeviceEnumerator::Enumerate: CoCreateInstance(CLSID_CDeckLinkIterator) failed");
        return devices;
    }

    int32_t subDeviceIndex = 0;
    IDeckLink* rawDeckLink = nullptr;
    while (iterator->Next(&rawDeckLink) == S_OK) {
        ComPtr<IDeckLink> deckLink;
        deckLink.Attach(rawDeckLink); // Next() hands back an already-AddRef'd pointer

        // GetDisplayName (unlike GetModelName) includes the per-connector disambiguator
        // ("DeckLink Duo 2 (1)", "(2)", ...) that Blackmagic's own tools (Desktop Video
        // Status, vMix, etc.) show — GetModelName alone returns the identical generic
        // model string for every sub-device of a multi-connector card, which is useless
        // for telling 4 "DeckLink Duo 2" dropdown entries apart.
        BSTR nameBstr = nullptr;
        std::wstring displayName = L"DeckLink";
        if (SUCCEEDED(deckLink->GetDisplayName(&nameBstr)) && nameBstr) {
            displayName = nameBstr;
            SysFreeString(nameBstr);
        } else if (SUCCEEDED(deckLink->GetModelName(&nameBstr)) && nameBstr) {
            displayName = nameBstr;
            SysFreeString(nameBstr);
        }

        ComPtr<IDeckLinkInput> input;
        int64_t persistentId = 0;
        const bool hasInput = TryGetInputAndPersistentId(deckLink.Get(), input, persistentId);
        LogDiagnostic(
            L"DeckLinkDeviceEnumerator::Enumerate: sub-device " + std::to_wstring(subDeviceIndex) + L" (" +
            displayName + L") " + (hasInput ? L"has input, persistentId=" + MakeDeviceId(persistentId)
                                             : L"has NO input on this profile (output-only, or GetInt(BMDDeckLinkPersistentID) failed)"));
        ++subDeviceIndex;
        if (!hasInput) {
            continue;
        }

        CcDeviceInfo info{};
        info.backend = CcBackendType::DeckLink;
        wcsncpy_s(info.id.value, CC_MAX_STRING, MakeDeviceId(persistentId).c_str(), _TRUNCATE);
        wcsncpy_s(info.displayName, CC_MAX_STRING, displayName.c_str(), _TRUNCATE);
        devices.push_back(info);
    }

    LogDiagnostic(
        L"DeckLinkDeviceEnumerator::Enumerate: " + std::to_wstring(subDeviceIndex) + L" sub-device(s) found, " +
        std::to_wstring(devices.size()) + L" with input capability");

    return devices;
}

std::unique_ptr<ICaptureSource> DeckLinkDeviceEnumerator::Open(const CcDeviceId& id, const CcCaptureFormat& format) {
    ComPtr<IDeckLinkIterator> iterator;
    if (FAILED(CoCreateInstance(
            CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL, IID_IDeckLinkIterator, &iterator))) {
        return nullptr;
    }

    IDeckLink* rawDeckLink = nullptr;
    while (iterator->Next(&rawDeckLink) == S_OK) {
        ComPtr<IDeckLink> deckLink;
        deckLink.Attach(rawDeckLink);

        ComPtr<IDeckLinkInput> input;
        int64_t persistentId = 0;
        if (!TryGetInputAndPersistentId(deckLink.Get(), input, persistentId)) {
            continue;
        }

        if (MakeDeviceId(persistentId) != id.value) {
            continue;
        }

        try {
            return std::make_unique<DeckLinkCaptureSource>(std::move(input), format);
        } catch (const std::exception&) {
            return nullptr;
        }
    }

    return nullptr;
}

} // namespace capturecore
