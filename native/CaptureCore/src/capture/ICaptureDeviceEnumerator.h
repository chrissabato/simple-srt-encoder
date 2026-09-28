#pragma once

#include "capturecore/capture_types.h"
#include "ICaptureSource.h"

#include <memory>
#include <vector>

namespace capturecore {

class ICaptureDeviceEnumerator {
public:
    virtual ~ICaptureDeviceEnumerator() = default;

    virtual CcBackendType Backend() const = 0;

    // False if this backend's SDK wasn't compiled in, or its runtime isn't present
    // (e.g. Blackmagic Desktop Video drivers not installed) — never throws.
    virtual bool IsAvailable() const = 0;

    virtual std::vector<CcDeviceInfo> Enumerate() = 0;

    // Opens a device previously returned by Enumerate(). Returns nullptr on failure.
    virtual std::unique_ptr<ICaptureSource> Open(const CcDeviceId& id, const CcCaptureFormat& format) = 0;
};

} // namespace capturecore
