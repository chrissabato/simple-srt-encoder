#pragma once

#include "../ICaptureSource.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <wrl/client.h>
#include <mfidl.h>
#include <mfreadwrite.h>

namespace capturecore {

// Captures BGRA32 frames from a UVC device via Windows Media Foundation's
// IMFSourceReader (synchronous pull mode, on a dedicated background thread). The
// source reader is configured with MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING so it
// inserts whatever decoder/color-converter MFTs are needed to hand us RGB32 frames
// regardless of the camera's native pixel format (typically NV12 or MJPEG).
class UvcCaptureSource : public ICaptureSource {
public:
    // Takes ownership of mediaSource (already activated by UvcDeviceEnumerator).
    // Throws std::runtime_error on failure to configure/start capture.
    UvcCaptureSource(Microsoft::WRL::ComPtr<IMFMediaSource> mediaSource, const CcCaptureFormat& requestedFormat);
    ~UvcCaptureSource() override;

    UvcCaptureSource(const UvcCaptureSource&) = delete;
    UvcCaptureSource& operator=(const UvcCaptureSource&) = delete;

    bool TryGetLatestFrame(CcFrameBuffer& frame) override;
    int32_t Width() const override { return m_width; }
    int32_t Height() const override { return m_height; }

private:
    void CaptureThreadMain();

    Microsoft::WRL::ComPtr<IMFMediaSource> m_mediaSource;
    Microsoft::WRL::ComPtr<IMFSourceReader> m_reader;

    std::thread m_captureThread;
    std::atomic<bool> m_stopRequested{false};

    std::mutex m_frameMutex;
    std::vector<uint8_t> m_latestFrameData;
    int64_t m_latestTimestamp100ns = 0;
    bool m_hasFrame = false;

    int32_t m_width = 0;
    int32_t m_height = 0;
    int32_t m_strideBytes = 0;       // stride of m_latestFrameData; always == m_width * 4
    int32_t m_sourceStrideBytes = 0; // stride of the raw MF buffer (may include padding)
    bool m_sourceIsBottomUp = false; // true if MF_MT_DEFAULT_STRIDE was negative
};

} // namespace capturecore
