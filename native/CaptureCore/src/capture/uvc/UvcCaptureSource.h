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

// Native pixel format of the locked source stream, so the capture thread knows which
// software conversion (if any) to apply when copying into the exposed BGRA32 buffer.
enum class UvcSourceFormat {
    Bgra32, // no conversion needed
    Yuy2,   // packed 4:2:2 (Y0 U Y1 V per pixel pair)
    Nv12,   // planar 4:2:0 (Y plane + interleaved UV plane at half resolution)
};

// Captures BGRA32 frames from a UVC device via Windows Media Foundation's
// IMFSourceReader (synchronous pull mode, on a dedicated background thread).
//
// Deliberately does NOT use MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING to have the
// source reader auto-insert a color-converter MFT: on at least one real device tested
// during development, the hardware-accelerated video processor MFT that gets
// auto-selected failed to start streaming at all (MF_E_HW_MFT_FAILED_START_STREAMING,
// 0xC00D3704) even though format negotiation itself succeeded — likely a GPU/driver
// resource issue specific to that machine, not something an app can reliably work
// around by asking nicely. Locking the camera's native format directly and converting
// YUY2/NV12 to BGRA32 in software here is slower per-frame but has no such external
// dependency, and only two formats need to be handled since virtually all UVC cameras
// report one of these two as their native/preferred format.
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
    // Set by CaptureThreadMain when ReadSample reports MF_SOURCE_READERF_ERROR (or any
    // other failure) — per IMFSourceReader's documented contract, no further calls to
    // it are safe once that happens. The destructor must check this before calling
    // Flush(); calling it anyway on this device's driver was the actual cause of a
    // 0xc000027b native crash on Close (the fault surfaces inside Microsoft.UI.Xaml.dll
    // as a generic "unhandled exception crossed a WinRT boundary" fail-fast, which is
    // why it looked XAML-related rather than obviously Media-Foundation-related).
    std::atomic<bool> m_readerFaulted{false};

    std::mutex m_frameMutex;
    std::vector<uint8_t> m_latestFrameData;
    int64_t m_latestTimestamp100ns = 0;
    bool m_hasFrame = false;

    int32_t m_width = 0;
    int32_t m_height = 0;
    int32_t m_strideBytes = 0;       // stride of m_latestFrameData; always == m_width * 4
    int32_t m_sourceStrideBytes = 0; // stride (of the Y/BGRA plane) in the raw MF buffer
    bool m_sourceIsBottomUp = false; // true if MF_MT_DEFAULT_STRIDE was negative (BGRA32 only)
    UvcSourceFormat m_sourceFormat = UvcSourceFormat::Bgra32;
};

} // namespace capturecore
