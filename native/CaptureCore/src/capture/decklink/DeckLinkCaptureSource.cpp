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

DeckLinkCaptureSource::DeckLinkCaptureSource(ComPtr<IDeckLinkInput> input, const CcCaptureFormat& requestedFormat)
    : m_input(std::move(input)) {
    LogDiagnostic(L"DeckLinkCaptureSource: picking display mode...");

    ComPtr<IDeckLinkDisplayModeIterator> modeIterator;
    if (FAILED(m_input->GetDisplayModeIterator(&modeIterator))) {
        throw std::runtime_error("GetDisplayModeIterator failed");
    }

    BMDDisplayMode chosenMode = bmdModeUnknown;
    BMDDisplayMode firstMode = bmdModeUnknown;
    ComPtr<IDeckLinkDisplayMode> displayMode;
    while (modeIterator->Next(&displayMode) == S_OK) {
        const BMDDisplayMode mode = displayMode->GetDisplayMode();
        const int32_t width = static_cast<int32_t>(displayMode->GetWidth());
        const int32_t height = static_cast<int32_t>(displayMode->GetHeight());
        LogDiagnostic(L"    mode: " + std::to_wstring(width) + L"x" + std::to_wstring(height));

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

    if (!StartCapture(chosenMode, bmdFormat8BitBGRA)) {
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
    if (!(notificationEvents & (bmdVideoInputDisplayModeChanged | bmdVideoInputColorspaceChanged))) {
        return S_OK;
    }

    LogDiagnostic(L"DeckLinkCaptureSource: VideoInputFormatChanged, restarting with detected mode");
    m_input->StopStreams();
    // Re-request BGRA32 regardless of the detected colorspace — the card still does
    // that conversion in hardware; only the display mode (size/frame rate) needs to
    // follow what was actually detected on the input signal.
    if (m_input->EnableVideoInput(newDisplayMode->GetDisplayMode(), bmdFormat8BitBGRA,
                                   bmdVideoInputEnableFormatDetection) != S_OK) {
        return E_FAIL;
    }
    if (m_input->StartStreams() != S_OK) {
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
    if (videoFrame->GetFlags() & bmdFrameHasNoInputSource) {
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

    std::lock_guard<std::mutex> lock(m_frameMutex);
    if (m_width != width || m_height != height) {
        // First frame, or a format change resized the signal.
        m_width = width;
        m_height = height;
        m_strideBytes = packedRowBytes;
        m_latestFrameData.resize(static_cast<size_t>(packedRowBytes) * static_cast<size_t>(height));
    }

    const uint8_t* src = static_cast<const uint8_t*>(data);
    if (sourceStride == packedRowBytes) {
        std::memcpy(m_latestFrameData.data(), src, m_latestFrameData.size());
    } else {
        for (int32_t row = 0; row < height; ++row) {
            std::memcpy(
                m_latestFrameData.data() + static_cast<size_t>(row) * packedRowBytes,
                src + static_cast<size_t>(row) * sourceStride, packedRowBytes);
        }
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
