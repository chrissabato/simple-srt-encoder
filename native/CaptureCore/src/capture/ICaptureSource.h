#pragma once

#include "capturecore/capture_types.h"

#include <cstddef>

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

    // Embedded audio (e.g. SDI/HDMI audio delivered alongside video by DeckLink). Always
    // 48kHz, 2-channel, signed 16-bit little-endian PCM (kEmbeddedAudio* below), so the
    // encode pipeline can describe it to ffmpeg without per-source negotiation. Sources
    // without embedded audio keep these defaults.
    static constexpr int32_t kEmbeddedAudioSampleRate = 48000;
    static constexpr int32_t kEmbeddedAudioChannels = 2;

    virtual bool SupportsEmbeddedAudio() const { return false; }

    // Buffering only happens while enabled, so audio from before streaming started is
    // never replayed at the start of a stream. Enabling clears any stale buffer.
    virtual void SetEmbeddedAudioCapture(bool /*enabled*/) {}

    // Moves up to maxBytes of buffered PCM into dest; returns bytes copied (0 if none).
    virtual size_t ReadEmbeddedAudio(uint8_t* /*dest*/, size_t /*maxBytes*/) { return 0; }
};

} // namespace capturecore
