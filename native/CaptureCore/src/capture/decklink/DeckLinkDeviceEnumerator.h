#pragma once

#include "../ICaptureDeviceEnumerator.h"

namespace capturecore {

// Enumerates Blackmagic DeckLink capture devices. Only compiled in when the DeckLink
// SDK was found at CMake configure time (see CMakeLists.txt) — see
// native/vendor/README.md for how to make it available.
class DeckLinkDeviceEnumerator : public ICaptureDeviceEnumerator {
public:
    CcBackendType Backend() const override { return CcBackendType::DeckLink; }

    // False if the DeckLink CLSID isn't registered on this machine, i.e. the
    // Blackmagic Desktop Video drivers aren't installed (SDK-compiled-in is not the
    // same as runtime-present — this is checked independently every call since drivers
    // could be installed after the app started).
    bool IsAvailable() const override;

    std::vector<CcDeviceInfo> Enumerate() override;
    std::unique_ptr<ICaptureSource> Open(const CcDeviceId& id, const CcCaptureFormat& format) override;
};

} // namespace capturecore
