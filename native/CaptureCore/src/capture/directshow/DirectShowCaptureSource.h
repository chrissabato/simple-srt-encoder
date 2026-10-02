#pragma once

#include "../ICaptureSource.h"
#include "DirectShowGuids.h"

#include <chrono>
#include <control.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include <wrl/client.h>

namespace capturecore {

// Captures BGRA32 frames from a DirectShow software capture filter (virtual cameras
// like OBS Virtual Camera or vMix Video — see DirectShowDeviceEnumerator for why these
// specifically, as opposed to real hardware, need this separate backend) via a
// minimal filter graph: source filter -> Sample Grabber -> Null Renderer. Frames
// arrive by callback (ISampleGrabberCB::BufferCB, invoked on a DirectShow worker
// thread the graph creates internally) rather than a dedicated pull thread of our
// own, unlike UvcCaptureSource — DirectShow's graph already runs one once
// IMediaControl::Run() starts it.
class DirectShowCaptureSource : public ICaptureSource {
public:
    // Takes ownership of sourceFilter (already bound by DirectShowDeviceEnumerator).
    // Throws std::runtime_error on failure to build the graph or start capture.
    DirectShowCaptureSource(Microsoft::WRL::ComPtr<IBaseFilter> sourceFilter, const CcCaptureFormat& requestedFormat);
    ~DirectShowCaptureSource() override;

    DirectShowCaptureSource(const DirectShowCaptureSource&) = delete;
    DirectShowCaptureSource& operator=(const DirectShowCaptureSource&) = delete;

    bool TryGetLatestFrame(CcFrameBuffer& frame) override;
    int32_t Width() const override { return m_width; }
    int32_t Height() const override { return m_height; }

    // Called from the sample grabber's callback (a DirectShow-owned thread).
    void OnBuffer(const uint8_t* data, long length);

private:
    class GrabberCallback;

    // Native format of the connected sample-grabber pin, so OnBuffer knows which
    // software conversion to apply when copying into the exposed BGRA32 buffer —
    // same idea as UvcSourceFormat in UvcCaptureSource.h.
    enum class SourceFormat { Rgb24, Rgb32, Yuy2, Uyvy };

    std::unique_ptr<GrabberCallback> m_callback;

    Microsoft::WRL::ComPtr<IBaseFilter> m_sourceFilter;
    Microsoft::WRL::ComPtr<IGraphBuilder> m_graph;
    Microsoft::WRL::ComPtr<ICaptureGraphBuilder2> m_graphBuilder;
    Microsoft::WRL::ComPtr<IBaseFilter> m_grabberFilter;
    Microsoft::WRL::ComPtr<ISampleGrabber> m_grabber;
    Microsoft::WRL::ComPtr<IBaseFilter> m_nullRenderer;
    Microsoft::WRL::ComPtr<IMediaControl> m_mediaControl;

    std::mutex m_frameMutex;
    std::vector<uint8_t> m_latestFrameData;
    bool m_hasFrame = false;

    int32_t m_width = 0;
    int32_t m_height = 0;
    int32_t m_strideBytes = 0; // stride of m_latestFrameData; always == m_width * 4
    bool m_sourceIsBottomUp = false;
    SourceFormat m_sourceFormat = SourceFormat::Rgb32;

    // Tracks OnBuffer's own conversion cost, separate from anything ffmpeg/NVENC-side —
    // see the long comment at its use site in the .cpp for why this needed measuring
    // directly rather than inferred from ffmpeg's progress stats.
    std::chrono::steady_clock::time_point m_lastConversionLogTime{};
    std::chrono::nanoseconds m_maxConversionTimeSinceLog{0};
    uint64_t m_conversionCountSinceLog = 0;
    std::chrono::nanoseconds m_totalConversionTimeSinceLog{0};
    std::chrono::nanoseconds m_maxLockWaitTimeSinceLog{0};
    std::chrono::nanoseconds m_totalLockWaitTimeSinceLog{0};
};

} // namespace capturecore
