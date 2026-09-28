#pragma once

#include "capturecore/capture_types.h"

namespace capturecore {

// An open, actively-capturing video source. Frames are always available for preview via
// TryGetLatestFrame independent of whether streaming has started, so the UI can preview
// a source before going live.
class ICaptureSource {
public:
    virtual ~ICaptureSource() = default;

    // Copies the most recent frame (BGRA32) into the caller-owned buffer described by
    // frame. frame->data/capacity must already be set. Returns false if no frame is
    // available yet or the buffer is too small.
    virtual bool TryGetLatestFrame(CcFrameBuffer& frame) = 0;

    virtual int32_t Width() const = 0;
    virtual int32_t Height() const = 0;
};

} // namespace capturecore
