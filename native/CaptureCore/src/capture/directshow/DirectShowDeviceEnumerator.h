#pragma once

#include "../ICaptureDeviceEnumerator.h"

namespace capturecore {

// Enumerates DirectShow video capture filters that Media Foundation's
// MFEnumDeviceSources (see UvcDeviceEnumerator) never sees: virtual cameras
// registered as plain software filters (device path "@device_sw_...", e.g. OBS
// Virtual Camera, vMix's virtual outputs) rather than PnP hardware devices
// ("@device_pnp_..."). Only @device_sw_ devices are returned — @device_pnp_ ones are
// deliberately excluded here since UvcDeviceEnumerator already reports them, and
// including them again here would duplicate every entry under a second device-id
// scheme.
class DirectShowDeviceEnumerator : public ICaptureDeviceEnumerator {
public:
    CcBackendType Backend() const override { return CcBackendType::DirectShow; }

    // No proprietary SDK involved (unlike DeckLink/NDI) — DirectShow ships with
    // Windows itself, so this backend is always available.
    bool IsAvailable() const override { return true; }

    std::vector<CcDeviceInfo> Enumerate() override;
    std::unique_ptr<ICaptureSource> Open(const CcDeviceId& id, const CcCaptureFormat& format) override;
};

} // namespace capturecore
