#pragma once

#include "../ICaptureSource.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <vector>
#include <wrl/client.h>
#include <DeckLinkAPI.h>

namespace capturecore {

// Captures BGRA32 frames from a Blackmagic DeckLink device.
//
// Unlike UvcCaptureSource, this has no capture thread of its own: the DeckLink driver
// invokes VideoInputFrameArrived() on its own internal thread whenever a frame is ready
// (a push/callback model, not a pull loop). Native 8-bit YUV 4:2:2 is requested from
// EnableVideoInput (not BGRA32 — real hardware testing on a DeckLink Duo 2 sub-device in
// its 4-independent-input profile showed on-card BGRA32 conversion silently never
// locking despite the mode otherwise being correctly detected and nominally supported),
// and converted to BGRA32 in software here — the same pattern already used by the UVC
// YUY2/NV12 path, for the same underlying reason.
//
// Implements IDeckLinkInputCallback (and therefore IUnknown) by hand — deliberately not
// using ATL's CComPtr/CComQIPtr (as Blackmagic's own samples do) to avoid depending on
// the optional "C++ ATL" Visual Studio component; Microsoft::WRL::ComPtr (already used
// throughout this project for Media Foundation) covers everything needed here too.
class DeckLinkCaptureSource : public ICaptureSource, public IDeckLinkInputCallback {
public:
    // Throws std::runtime_error on failure to configure/start capture, or if no frame
    // arrives within a few seconds of starting (e.g. no signal present).
    DeckLinkCaptureSource(Microsoft::WRL::ComPtr<IDeckLinkInput> input, const CcCaptureFormat& requestedFormat);
    ~DeckLinkCaptureSource() override;

    DeckLinkCaptureSource(const DeckLinkCaptureSource&) = delete;
    DeckLinkCaptureSource& operator=(const DeckLinkCaptureSource&) = delete;

    bool TryGetLatestFrame(CcFrameBuffer& frame) override;
    int32_t Width() const override { return m_width; }
    int32_t Height() const override { return m_height; }

    bool SupportsEmbeddedAudio() const override { return m_audioInputEnabled; }
    void SetEmbeddedAudioCapture(bool enabled) override;
    size_t ReadEmbeddedAudio(uint8_t* dest, size_t maxBytes) override;

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IDeckLinkInputCallback
    HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(
        BMDVideoInputFormatChangedEvents notificationEvents, IDeckLinkDisplayMode* newDisplayMode,
        BMDDetectedVideoInputFormatFlags detectedSignalFlags) override;
    HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(
        IDeckLinkVideoInputFrame* videoFrame, IDeckLinkAudioInputPacket* audioPacket) override;

private:
    bool StartCapture(BMDDisplayMode displayMode, BMDPixelFormat pixelFormat);
    void StopCapture();

    std::atomic<ULONG> m_refCount{1};

    Microsoft::WRL::ComPtr<IDeckLinkInput> m_input;
    bool m_formatDetectionSupported = false;

    std::mutex m_frameMutex;
    std::vector<uint8_t> m_latestFrameData;
    int64_t m_latestTimestamp100ns = 0;
    bool m_hasFrame = false;
    std::chrono::steady_clock::time_point m_lastNoSignalLog{};
    std::chrono::steady_clock::time_point m_lastQueueDepthLog{};
    std::chrono::steady_clock::time_point m_lastConversionTimingLog{};
    double m_maxConversionMsThisWindow = 0.0;

    int32_t m_width = 0;
    int32_t m_height = 0;
    int32_t m_strideBytes = 0;

    bool m_audioInputEnabled = false;
    std::mutex m_audioMutex;
    std::deque<uint8_t> m_audioBuffer; // guarded by m_audioMutex
    bool m_audioCaptureActive = false; // guarded by m_audioMutex
    std::chrono::steady_clock::time_point m_lastAudioOverflowLog{}; // guarded by m_audioMutex
};

} // namespace capturecore
