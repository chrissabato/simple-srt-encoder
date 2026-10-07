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
// for a physical device across enumerations, so it's preferred as the opaque CcDeviceId
// when available. Real hardware testing found it's NOT always available, though (see
// MakeFallbackDeviceId below) — don't assume every DeckLink device reports one.
std::wstring MakeDeviceId(int64_t persistentId) {
    std::wstringstream ss;
    ss << L"decklink:" << std::hex << persistentId;
    return ss.str();
}

// Fallback identifier keyed on enumeration order, used when BMDDeckLinkPersistentID
// isn't available (see the comment on hasPersistentId below) — stable enough across an
// Enumerate() and a subsequent Open() since both do a fresh CoCreateInstance +
// IDeckLinkIterator walk in the same hardware-topology order each time, the same
// assumption the persistent-ID path already implicitly relies on.
std::wstring MakeFallbackDeviceId(int32_t subDeviceIndex) {
    return L"decklink:idx" + std::to_wstring(subDeviceIndex);
}

// outPersistentId/outHasPersistentId are only meaningful when this returns true.
bool TryGetInput(IDeckLink* deckLink, ComPtr<IDeckLinkInput>& outInput) {
    ComPtr<IDeckLink> device(deckLink);
    if (SUCCEEDED(device.As(&outInput))) {
        return true;
    }

    // Fallback for older installed Desktop Video drivers: confirmed on a real DeckLink
    // Mini Recorder HD running Desktop Video 15.0, while this repo's bundled SDK (see
    // DeckLinkAPIVersion.h) is from SDK 16.0 — the *current* IID_IDeckLinkInput this QFI
    // requests above isn't implemented by that older driver at all (QueryInterface just
    // fails outright; it's a version mismatch, not an "output-only device" the earlier,
    // less specific error message implied). SDK 16.0 still ships the exact interface
    // shape frozen at SDK 15.3.1 for this (IDeckLinkInput_v15_3_1) — verified by
    // comparing both generated headers line-for-line that every method this project
    // actually calls (GetDisplayModeIterator, EnableVideoInput, EnableAudioInput,
    // DisableAudioInput, Start/Stop/Pause/FlushStreams, SetCallback,
    // GetAvailableVideoFrameCount, DisableVideoInput) has an identical vtable slot and
    // signature in both versions, AND that v15.3.1's SetCallback already takes today's
    // unversioned IDeckLinkInputCallback* directly (the callback interface itself was
    // last changed exactly at 15.3.1) — so DeckLinkCaptureSource needs no changes to work
    // against either generation through this same ComPtr<IDeckLinkInput>. Deliberately
    // does NOT fall back further than this: older generations (_v14_2_1 and earlier)
    // changed the callback interface's VideoInputFrameArrived parameter type too, which
    // would need a real adapter, not just a different QueryInterface call.
    if (SUCCEEDED(device->QueryInterface(
            IID_IDeckLinkInput_v15_3_1, reinterpret_cast<void**>(outInput.ReleaseAndGetAddressOf())))) {
        LogDiagnostic(
            L"DeckLinkDeviceEnumerator: device only implements IDeckLinkInput_v15_3_1, not the current SDK "
            L"16.0 IDeckLinkInput — falling back to it (older Desktop Video driver installed)");
        return true;
    }

    return false; // output-only device, no input on this profile, or an even older driver
}

// Real hardware testing found a DeckLink Mini Recorder HD (a genuinely input-only
// product — there's no ambiguity about whether it "has input") returning a
// BMDDeckLinkPersistentID of 0 / failing the attribute query entirely, apparently
// depending on the Desktop Video driver version — unlike the DeckLink Duo 2 this was
// originally tested against, where every sub-device reported a real non-zero ID. This
// single device's real identity doesn't need distinguishing from any other
// (BMDDeckLinkPersistentID only matters for telling multiple sub-devices apart in the
// first place), so treating "no persistent ID" as "not an input device" was wrong — it
// silently excluded a perfectly good, genuinely input-capable device from the list.
bool TryGetPersistentId(IDeckLink* deckLink, int64_t& outPersistentId) {
    ComPtr<IDeckLink> device(deckLink);
    ComPtr<IDeckLinkProfileAttributes> attributes;
    if (FAILED(device.As(&attributes))) {
        return false;
    }
    outPersistentId = 0;
    return SUCCEEDED(attributes->GetInt(BMDDeckLinkPersistentID, &outPersistentId)) && outPersistentId != 0;
}

} // namespace

bool DeckLinkDeviceEnumerator::IsAvailable() const {
    ComPtr<IDeckLinkIterator> iterator;
    const HRESULT hr = CoCreateInstance(
        CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL, IID_IDeckLinkIterator, &iterator);
    if (FAILED(hr)) {
        // Logged unconditionally (not just on first failure) since this is cheap and is
        // the only signal that explains "DeckLink hardware is installed but none of our
        // DeckLink-specific devices show up" — e.g. REGDB_E_CLASSNOTREG (0x80040154)
        // means the DeckLink API COM class itself isn't registered for this process's
        // bitness, which happens with some older/mismatched Desktop Video driver
        // installs even though a legacy DirectShow capture filter still shows up fine
        // via the separate DirectShowDeviceEnumerator.
        std::wstringstream ss;
        ss << L"DeckLinkDeviceEnumerator::IsAvailable: CoCreateInstance(CLSID_CDeckLinkIterator) failed, hr=0x"
           << std::hex << static_cast<unsigned long>(hr);
        LogDiagnostic(ss.str());
    }
    return SUCCEEDED(hr);
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
        const bool hasInput = TryGetInput(deckLink.Get(), input);
        if (!hasInput) {
            LogDiagnostic(
                L"DeckLinkDeviceEnumerator::Enumerate: sub-device " + std::to_wstring(subDeviceIndex) + L" (" +
                displayName + L") has NO input on this profile (output-only device)");
            ++subDeviceIndex;
            continue;
        }

        int64_t persistentId = 0;
        const bool hasPersistentId = TryGetPersistentId(deckLink.Get(), persistentId);
        const std::wstring deviceId = hasPersistentId ? MakeDeviceId(persistentId) : MakeFallbackDeviceId(subDeviceIndex);
        LogDiagnostic(
            L"DeckLinkDeviceEnumerator::Enumerate: sub-device " + std::to_wstring(subDeviceIndex) + L" (" +
            displayName + L") has input, id=" + deviceId +
            (hasPersistentId ? L"" : L" (no BMDDeckLinkPersistentID reported — using enumeration-order fallback)"));
        ++subDeviceIndex;

        CcDeviceInfo info{};
        info.backend = CcBackendType::DeckLink;
        wcsncpy_s(info.id.value, CC_MAX_STRING, deviceId.c_str(), _TRUNCATE);
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

    int32_t subDeviceIndex = 0;
    IDeckLink* rawDeckLink = nullptr;
    while (iterator->Next(&rawDeckLink) == S_OK) {
        ComPtr<IDeckLink> deckLink;
        deckLink.Attach(rawDeckLink);

        ComPtr<IDeckLinkInput> input;
        if (!TryGetInput(deckLink.Get(), input)) {
            ++subDeviceIndex;
            continue;
        }

        int64_t persistentId = 0;
        const bool hasPersistentId = TryGetPersistentId(deckLink.Get(), persistentId);
        const std::wstring deviceId = hasPersistentId ? MakeDeviceId(persistentId) : MakeFallbackDeviceId(subDeviceIndex);
        ++subDeviceIndex;

        if (deviceId != id.value) {
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
