#include "DeckLinkCaptureSource.h"
#include "../../Diagnostics.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace capturecore {

namespace {

// bmdFormat8BitYUV is packed 4:2:2, byte order U0 Y0 V0 Y1 (the traditional
// broadcast/SDI packing, same as DirectShowCaptureSource's UYVY path — see
// ConvertUyvyRowToBgra32 there for the identical byte-order comment). Requesting this
// native format instead of asking the card to convert to BGRA32 on-board turned out to
// be necessary on real hardware (see the comment in StartCapture): a DeckLink Duo 2
// sub-device running in its bandwidth-reduced 4-independent-input profile would
// correctly auto-detect and "accept" a BGRA32 request for a real, cable-confirmed signal
// (DoesSupportVideoMode even reported it as supported) yet every frame still came back
// flagged bmdFrameHasNoInputSource forever — the on-card YUV->RGB conversion silently
// never actually locked in that reduced-resource profile. Capturing native YUV (half the
// per-pixel bandwidth of BGRA32, no on-card conversion at all) and converting to BGRA32
// in software here instead is the same fix already proven necessary for UVC/DirectShow
// sources on this project, applied to the same underlying class of problem.
//
// Uses the same precomputed-lookup-table ITU-R BT.601 math as
// DirectShowCaptureSource.cpp's ConvertUyvyRowToBgra32 (see that file's comment for the
// field-test numbers), not a naive per-pixel multiply — a first version of this function
// used scalar multiplication directly and could not keep up with 1280x720@59.94fps in a
// real test against a DeckLink Duo 2 (confirmed via a vMix control test on the identical
// signal path showing no delay, ruling out the camera/converter): DeckLink's internal
// frame queue backed up because conversion was slower than the incoming rate, so every
// "latest" frame handed to the UI was in fact several seconds stale and never caught up.
uint8_t ClampToByte(int value) {
    return static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
}

struct YuvToBgrTables {
    int y[256];
    int uToB[256];
    int uToG[256];
    int vToR[256];
    int vToG[256];

    YuvToBgrTables() {
        for (int i = 0; i < 256; ++i) {
            const int c = i - 16;
            const int d = i - 128;
            const int e = i - 128;
            y[i] = 298 * c;
            uToB[i] = 516 * d;
            uToG[i] = -100 * d;
            vToR[i] = 409 * e;
            vToG[i] = -208 * e;
        }
    }
};

const YuvToBgrTables& GetYuvToBgrTables() {
    static const YuvToBgrTables tables; // thread-safe init (C++11 magic statics)
    return tables;
}

void YuvToBgr(int y, int u, int v, uint8_t& b, uint8_t& g, uint8_t& r) {
    const YuvToBgrTables& t = GetYuvToBgrTables();
    const int yTerm = t.y[y];
    r = ClampToByte((yTerm + t.vToR[v] + 128) >> 8);
    g = ClampToByte((yTerm + t.uToG[u] + t.vToG[v] + 128) >> 8);
    b = ClampToByte((yTerm + t.uToB[u] + 128) >> 8);
}

void ConvertUyvyRowToBgra32(const uint8_t* srcRow, uint8_t* dstRow, int32_t width) {
    for (int32_t x = 0; x + 1 < width; x += 2) {
        const uint8_t* px = srcRow + static_cast<size_t>(x) * 2;
        const int u = px[0], y0 = px[1], v = px[2], y1 = px[3];

        uint8_t b, g, r;
        YuvToBgr(y0, u, v, b, g, r);
        uint8_t* dst0 = dstRow + static_cast<size_t>(x) * 4;
        dst0[0] = b;
        dst0[1] = g;
        dst0[2] = r;
        dst0[3] = 0xFF;

        YuvToBgr(y1, u, v, b, g, r);
        uint8_t* dst1 = dstRow + static_cast<size_t>(x + 1) * 4;
        dst1[0] = b;
        dst1[1] = g;
        dst1[2] = r;
        dst1[3] = 0xFF;
    }
}

} // namespace

DeckLinkCaptureSource::DeckLinkCaptureSource(ComPtr<IDeckLinkInput> input, const CcCaptureFormat& requestedFormat)
    : m_input(std::move(input)) {
    LogDiagnostic(L"DeckLinkCaptureSource: picking display mode...");

    ComPtr<IDeckLinkDisplayModeIterator> modeIterator;
    if (FAILED(m_input->GetDisplayModeIterator(&modeIterator))) {
        throw std::runtime_error("GetDisplayModeIterator failed");
    }

    LogDiagnostic(
        L"  requested format: " + std::to_wstring(requestedFormat.width) + L"x" +
        std::to_wstring(requestedFormat.height));

    BMDDisplayMode chosenMode = bmdModeUnknown;
    BMDDisplayMode firstMode = bmdModeUnknown;
    ComPtr<IDeckLinkDisplayMode> displayMode;
    while (modeIterator->Next(&displayMode) == S_OK) {
        const BMDDisplayMode mode = displayMode->GetDisplayMode();
        const int32_t width = static_cast<int32_t>(displayMode->GetWidth());
        const int32_t height = static_cast<int32_t>(displayMode->GetHeight());
        BSTR modeName = nullptr;
        std::wstring modeNameStr;
        if (SUCCEEDED(displayMode->GetName(&modeName)) && modeName) {
            modeNameStr = modeName;
            SysFreeString(modeName);
        }
        LogDiagnostic(
            L"    mode: " + std::to_wstring(width) + L"x" + std::to_wstring(height) + L" (" + modeNameStr + L")");

        if (firstMode == bmdModeUnknown) {
            firstMode = mode;
        }
        if (requestedFormat.width > 0 && requestedFormat.height > 0 && width == requestedFormat.width &&
            height == requestedFormat.height) {
            chosenMode = mode;
        }
        displayMode.Reset();
        if (chosenMode != bmdModeUnknown) {
            break;
        }
    }
    if (chosenMode == bmdModeUnknown) {
        chosenMode = firstMode;
        LogDiagnostic(L"  no mode matched the requested size; falling back to the first enumerated mode");
    }
    if (chosenMode == bmdModeUnknown) {
        throw std::runtime_error("DeckLink device exposes no display modes");
    }

    ComPtr<IDeckLinkProfileAttributes> attributes;
    if (SUCCEEDED(m_input.As(&attributes))) {
        BOOL supported = FALSE;
        if (SUCCEEDED(attributes->GetFlag(BMDDeckLinkSupportsInputFormatDetection, &supported))) {
            m_formatDetectionSupported = (supported != FALSE);
        }
    }
    LogDiagnostic(
        std::wstring(L"  format auto-detection supported: ") + (m_formatDetectionSupported ? L"yes" : L"no"));

    if (!StartCapture(chosenMode, bmdFormat8BitYUV)) {
        throw std::runtime_error("Failed to start DeckLink capture");
    }

    // Wait briefly for the first frame (or timeout, e.g. no input signal present) so
    // OpenSource() correctly reports failure rather than "succeeding" into a source
    // that will never actually produce anything — mirrors the same fix applied to
    // UvcCaptureSource, and for the same underlying reason (a source that negotiates
    // successfully but never streams is worse than one that fails outright).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool gotFrame = false;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            gotFrame = m_hasFrame;
        }
        if (gotFrame) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (!gotFrame) {
        LogDiagnostic(L"  no frame within timeout (no input signal present?)");
        StopCapture();
        throw std::runtime_error("DeckLink device did not produce a frame (no input signal?)");
    }
}

DeckLinkCaptureSource::~DeckLinkCaptureSource() {
    StopCapture();
}

bool DeckLinkCaptureSource::StartCapture(BMDDisplayMode displayMode, BMDPixelFormat pixelFormat) {
    BMDVideoInputFlags flags = bmdVideoInputFlagDefault;
    if (m_formatDetectionSupported) {
        flags |= bmdVideoInputEnableFormatDetection;
    }

    m_input->SetCallback(this);

    if (m_input->EnableVideoInput(displayMode, pixelFormat, flags) != S_OK) {
        LogDiagnostic(L"  EnableVideoInput failed");
        return false;
    }
    // Audio is best-effort: a card/signal that can't deliver 2ch 48kHz PCM just means
    // this source reports no embedded audio, not that video capture fails.
    m_audioInputEnabled = SUCCEEDED(m_input->EnableAudioInput(
        bmdAudioSampleRate48kHz, bmdAudioSampleType16bitInteger,
        static_cast<uint32_t>(kEmbeddedAudioChannels)));
    LogDiagnostic(std::wstring(L"  EnableAudioInput ") + (m_audioInputEnabled ? L"ok" : L"failed"));

    if (m_input->StartStreams() != S_OK) {
        LogDiagnostic(L"  StartStreams failed");
        if (m_audioInputEnabled) {
            m_input->DisableAudioInput();
            m_audioInputEnabled = false;
        }
        m_input->DisableVideoInput();
        return false;
    }
    return true;
}

void DeckLinkCaptureSource::StopCapture() {
    if (m_input) {
        m_input->StopStreams();
        // Unregister before this object can be destroyed — DeckLink holds this pointer
        // as a borrowed reference (see the Release() comment below), not a ref-counted
        // one, so it must never be allowed to fire after we're gone.
        m_input->SetCallback(nullptr);
        if (m_audioInputEnabled) {
            m_input->DisableAudioInput();
            m_audioInputEnabled = false;
        }
        m_input->DisableVideoInput();
    }
}

void DeckLinkCaptureSource::SetEmbeddedAudioCapture(bool enabled) {
    std::lock_guard<std::mutex> lock(m_audioMutex);
    m_audioCaptureActive = enabled;
    m_audioBuffer.clear();
}

size_t DeckLinkCaptureSource::ReadEmbeddedAudio(uint8_t* dest, size_t maxBytes) {
    std::lock_guard<std::mutex> lock(m_audioMutex);
    // Whole sample frames only, so a partial frame is never split across pipe writes.
    constexpr size_t kBytesPerFrame = static_cast<size_t>(kEmbeddedAudioChannels) * 2;
    const size_t available = std::min(m_audioBuffer.size(), maxBytes);
    const size_t count = available - (available % kBytesPerFrame);
    std::copy_n(m_audioBuffer.begin(), count, dest);
    m_audioBuffer.erase(m_audioBuffer.begin(), m_audioBuffer.begin() + static_cast<std::ptrdiff_t>(count));
    return count;
}

HRESULT DeckLinkCaptureSource::QueryInterface(REFIID iid, void** ppv) {
    if (!ppv) {
        return E_INVALIDARG;
    }
    *ppv = nullptr;
    if (iid == IID_IUnknown) {
        *ppv = static_cast<IUnknown*>(static_cast<IDeckLinkInputCallback*>(this));
    } else if (iid == IID_IDeckLinkInputCallback) {
        *ppv = static_cast<IDeckLinkInputCallback*>(this);
    } else {
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

ULONG DeckLinkCaptureSource::AddRef() {
    return ++m_refCount;
}

ULONG DeckLinkCaptureSource::Release() {
    // Deliberately never `delete this` here: this object's real lifetime is owned by a
    // std::unique_ptr<ICaptureSource> (see UvcDeviceEnumerator::Open's DeckLink
    // equivalent in DeckLinkDeviceEnumerator::Open), not by COM refcounting. DeckLink
    // only ever holds this pointer as a borrowed reference via SetCallback — it doesn't
    // AddRef it on registration — and StopCapture() always clears that with
    // SetCallback(nullptr) before the unique_ptr can destroy us. The refcount here
    // exists to satisfy IUnknown's contract, not to drive deletion.
    return --m_refCount;
}

HRESULT DeckLinkCaptureSource::VideoInputFormatChanged(
    BMDVideoInputFormatChangedEvents notificationEvents, IDeckLinkDisplayMode* newDisplayMode,
    BMDDetectedVideoInputFormatFlags /*detectedSignalFlags*/) {
    // Only an actual display-mode (size/frame-rate) change requires restarting the
    // stream with a new buffer geometry. A colorspace-only notification does NOT: we
    // always request bmdFormat8BitBGRA regardless of the input's native colorspace (the
    // card does that conversion on-board either way — see the comment below). Some
    // sources/profiles report a colorspace-changed flag on every single frame (observed
    // on a DeckLink Duo 2 sub-device running in its 4-independent-input profile) —
    // restarting on that flag turned this into an infinite restart storm (StopStreams/
    // EnableVideoInput/StartStreams every ~33ms) that never let a single frame reach
    // VideoInputFrameArrived, so OpenSource always hit its 3s "no frame" timeout and
    // failed, which looked like a black preview with an easy-to-miss status message.
    if (!(notificationEvents & bmdVideoInputDisplayModeChanged)) {
        return S_OK;
    }

    BSTR modeName = nullptr;
    std::wstring modeNameStr;
    if (SUCCEEDED(newDisplayMode->GetName(&modeName)) && modeName) {
        modeNameStr = modeName;
        SysFreeString(modeName);
    }
    LogDiagnostic(L"DeckLinkCaptureSource: VideoInputFormatChanged, restarting with detected mode " + modeNameStr);

    // PauseStreams (not StopStreams) + FlushStreams between EnableVideoInput and
    // StartStreams, matching Blackmagic's own documented sequence (see
    // native/vendor/DeckLinkSDK/Examples/AutomaticModeDetection.cpp) — StopStreams tears
    // the stream down fully rather than handing it over cleanly for a format change, and
    // skipping FlushStreams left stale/queued frame state behind.
    m_input->PauseStreams();
    // Re-request native 8-bit YUV (not BGRA32 — see the namespace-scope comment above
    // ConvertUyvyRowToBgra32 for why) regardless of the detected colorspace; only the
    // display mode (size/frame rate) needs to follow what was actually detected.
    if (m_input->EnableVideoInput(newDisplayMode->GetDisplayMode(), bmdFormat8BitYUV,
                                   bmdVideoInputEnableFormatDetection) != S_OK) {
        LogDiagnostic(L"  EnableVideoInput (post-restart) failed");
        return E_FAIL;
    }
    m_input->FlushStreams();
    if (m_input->StartStreams() != S_OK) {
        LogDiagnostic(L"  StartStreams (post-restart) failed");
        return E_FAIL;
    }
    return S_OK;
}

HRESULT DeckLinkCaptureSource::VideoInputFrameArrived(
    IDeckLinkVideoInputFrame* videoFrame, IDeckLinkAudioInputPacket* audioPacket) {
    if (audioPacket) {
        void* audioData = nullptr;
        const long sampleFrames = audioPacket->GetSampleFrameCount();
        if (sampleFrames > 0 && SUCCEEDED(audioPacket->GetBytes(&audioData)) && audioData) {
            const size_t byteCount = static_cast<size_t>(sampleFrames) * kEmbeddedAudioChannels * 2;
            // Cap at ~2s so a stalled consumer (blocked ffmpeg pipe) can't grow this without bound.
            constexpr size_t kMaxBufferedBytes =
                static_cast<size_t>(kEmbeddedAudioSampleRate) * kEmbeddedAudioChannels * 2 * 2;
            std::lock_guard<std::mutex> lock(m_audioMutex);
            if (m_audioCaptureActive) {
                const auto* bytes = static_cast<const uint8_t*>(audioData);
                m_audioBuffer.insert(m_audioBuffer.end(), bytes, bytes + byteCount);
                if (m_audioBuffer.size() > kMaxBufferedBytes) {
                    m_audioBuffer.erase(
                        m_audioBuffer.begin(),
                        m_audioBuffer.begin() + static_cast<std::ptrdiff_t>(m_audioBuffer.size() - kMaxBufferedBytes));
                }
            }
        }
    }

    if (!videoFrame) {
        return S_OK;
    }

    // Diagnostic only (throttled to once/sec): if the DeckLink driver itself is holding
    // a deep internal backlog of already-captured frames (rather than always handing us
    // the newest one), this will show a large/growing count — which would explain a
    // fixed multi-second preview delay despite our own TryGetLatestFrame being a
    // zero-queue "always show the newest frame" overwrite that can't itself backlog.
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - m_lastQueueDepthLog > std::chrono::seconds(1)) {
            m_lastQueueDepthLog = now;
            unsigned int availableFrameCount = 0;
            if (SUCCEEDED(m_input->GetAvailableVideoFrameCount(&availableFrameCount))) {
                LogDiagnostic(L"DeckLinkCaptureSource: GetAvailableVideoFrameCount=" + std::to_wstring(availableFrameCount));
            }
        }
    }

    if (videoFrame->GetFlags() & bmdFrameHasNoInputSource) {
        // Throttled (once/sec) so a genuinely signal-less connector doesn't flood the
        // log at the frame rate, while still proving frames ARE arriving (distinguishes
        // "card enabled but nothing plugged into this connector" from "nothing is
        // calling this callback at all").
        const auto now = std::chrono::steady_clock::now();
        if (now - m_lastNoSignalLog > std::chrono::seconds(1)) {
            m_lastNoSignalLog = now;
            LogDiagnostic(L"DeckLinkCaptureSource: frame arrived flagged bmdFrameHasNoInputSource");
        }
        return S_OK; // card is enabled but no signal is present; nothing to copy yet
    }

    // SDK 16.0 moved raw pixel access off IDeckLinkVideoFrame onto a separate
    // IDeckLinkVideoBuffer interface with an explicit lock/unlock pattern (added to
    // support buffers that aren't plain host memory, e.g. GPUDirect). A plain
    // IDeckLinkVideoInputFrame from EnableVideoInput without GPUDirect is host memory
    // for the whole callback's duration regardless, but the StartAccess/EndAccess calls
    // are still required to get a valid pointer out of GetBytes.
    ComPtr<IDeckLinkVideoBuffer> videoBuffer;
    if (FAILED(videoFrame->QueryInterface(
            IID_IDeckLinkVideoBuffer, reinterpret_cast<void**>(videoBuffer.GetAddressOf())))) {
        return S_OK;
    }
    if (FAILED(videoBuffer->StartAccess(bmdBufferAccessRead))) {
        return S_OK;
    }
    void* data = nullptr;
    const HRESULT getBytesResult = videoBuffer->GetBytes(&data);
    if (FAILED(getBytesResult) || !data) {
        videoBuffer->EndAccess(bmdBufferAccessRead);
        return S_OK;
    }

    const int32_t width = static_cast<int32_t>(videoFrame->GetWidth());
    const int32_t height = static_cast<int32_t>(videoFrame->GetHeight());
    const int32_t sourceStride = static_cast<int32_t>(videoFrame->GetRowBytes());
    const int32_t packedRowBytes = width * 4;
    const BMDPixelFormat actualPixelFormat = videoFrame->GetPixelFormat();

    std::lock_guard<std::mutex> lock(m_frameMutex);
    if (m_width != width || m_height != height) {
        // First frame, or a format change resized the signal.
        m_width = width;
        m_height = height;
        m_strideBytes = packedRowBytes;
        m_latestFrameData.resize(static_cast<size_t>(packedRowBytes) * static_cast<size_t>(height));
        LogDiagnostic(
            L"DeckLinkCaptureSource: first/resized frame " + std::to_wstring(width) + L"x" +
            std::to_wstring(height) + L", pixelFormat=0x" + [actualPixelFormat] {
                std::wstringstream ss;
                ss << std::hex << actualPixelFormat;
                return ss.str();
            }() +
            L", sourceStride=" + std::to_wstring(sourceStride) + L" (expected width*2=" +
            std::to_wstring(width * 2) + L")");
    }

    // Source is native 8-bit YUV 4:2:2 (UYVY), 2 bytes/pixel — not BGRA32 — see the
    // ConvertUyvyRowToBgra32 comment above for why this is requested instead of asking
    // the card to convert on-board.
    const uint8_t* src = static_cast<const uint8_t*>(data);
    for (int32_t row = 0; row < height; ++row) {
        ConvertUyvyRowToBgra32(
            src + static_cast<size_t>(row) * sourceStride,
            m_latestFrameData.data() + static_cast<size_t>(row) * packedRowBytes, width);
    }

    m_hasFrame = true;
    // DeckLink doesn't hand back a simple host-clock timestamp through this callback;
    // frames are only ever consumed as "whatever's latest" (see TryGetLatestFrame
    // callers), not scheduled by original PTS, so this isn't needed for correctness.
    m_latestTimestamp100ns = 0;

    videoBuffer->EndAccess(bmdBufferAccessRead);
    return S_OK;
}

bool DeckLinkCaptureSource::TryGetLatestFrame(CcFrameBuffer& frame) {
    std::lock_guard<std::mutex> lock(m_frameMutex);
    if (!m_hasFrame) {
        return false;
    }
    const size_t requiredBytes = m_latestFrameData.size();
    if (frame.data == nullptr || static_cast<size_t>(frame.capacity) < requiredBytes) {
        return false;
    }
    std::memcpy(frame.data, m_latestFrameData.data(), requiredBytes);
    frame.width = m_width;
    frame.height = m_height;
    frame.strideBytes = m_strideBytes;
    frame.timestamp100ns = m_latestTimestamp100ns;
    return true;
}

} // namespace capturecore
