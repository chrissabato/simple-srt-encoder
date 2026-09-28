#pragma once

#include "../ICaptureDeviceEnumerator.h"

namespace capturecore {

// Enumerates UVC (and other Media Foundation video capture) devices. Always available —
// no proprietary SDK needed, just the Windows Media Foundation platform APIs.
class UvcDeviceEnumerator : public ICaptureDeviceEnumerator {
public:
    CcBackendType Backend() const override { return CcBackendType::Uvc; }
    bool IsAvailable() const override { return true; }

    std::vector<CcDeviceInfo> Enumerate() override;
    std::unique_ptr<ICaptureSource> Open(const CcDeviceId& id, const CcCaptureFormat& format) override;
};

} // namespace capturecore
