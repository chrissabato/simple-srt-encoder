#include "UvcCaptureSource.h"

#include <mfapi.h>
#include <mferror.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace capturecore {

namespace {

// Picks the native media type on the video stream whose frame size best matches the
// requested one (or the first native type if none is requested / none matches).
ComPtr<IMFMediaType> PickNativeType(IMFSourceReader* reader, const CcCaptureFormat& requested) {
    ComPtr<IMFMediaType> best;
    UINT32 bestWidth = 0, bestHeight = 0;

    for (DWORD typeIndex = 0;; ++typeIndex) {
        ComPtr<IMFMediaType> candidate;
        HRESULT hr = reader->GetNativeMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), typeIndex, &candidate);
        if (hr == MF_E_NO_MORE_TYPES || FAILED(hr)) {
            break;
        }

        UINT32 width = 0, height = 0;
        MFGetAttributeSize(candidate.Get(), MF_MT_FRAME_SIZE, &width, &height);

        if (!best) {
            best = candidate;
            bestWidth = width;
            bestHeight = height;
        }

        if (requested.width > 0 && requested.height > 0 &&
            static_cast<int32_t>(width) == requested.width &&
            static_cast<int32_t>(height) == requested.height) {
            return candidate;
        }
    }

    if (!best) {
        throw std::runtime_error("UVC device exposes no native media types");
    }
    return best;
}

} // namespace

UvcCaptureSource::UvcCaptureSource(ComPtr<IMFMediaSource> mediaSource, const CcCaptureFormat& requestedFormat)
    : m_mediaSource(std::move(mediaSource)) {
    ComPtr<IMFAttributes> readerAttributes;
    HRESULT hr = MFCreateAttributes(&readerAttributes, 1);
    if (FAILED(hr)) {
        throw std::runtime_error("MFCreateAttributes failed");
    }
    // Lets the source reader insert whatever decoder/color-converter MFTs are needed
    // to produce the RGB32 output type we request below, regardless of the camera's
    // native format (NV12, MJPEG, YUY2, ...).
    readerAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    hr = MFCreateSourceReaderFromMediaSource(m_mediaSource.Get(), readerAttributes.Get(), &m_reader);
    if (FAILED(hr)) {
        throw std::runtime_error("MFCreateSourceReaderFromMediaSource failed");
    }

    ComPtr<IMFMediaType> nativeType = PickNativeType(m_reader.Get(), requestedFormat);

    ComPtr<IMFMediaType> outputType;
    hr = MFCreateMediaType(&outputType);
    if (FAILED(hr)) {
        throw std::runtime_error("MFCreateMediaType failed");
    }
    hr = outputType->CopyAllItems(nativeType.Get());
    if (FAILED(hr)) {
        throw std::runtime_error("IMFMediaType::CopyAllItems failed");
    }
    outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);

    hr = m_reader->SetCurrentMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, outputType.Get());
    if (FAILED(hr)) {
        throw std::runtime_error("SetCurrentMediaType(RGB32) failed - camera may not support conversion");
    }

    ComPtr<IMFMediaType> actualType;
    hr = m_reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &actualType);
    if (FAILED(hr)) {
        throw std::runtime_error("GetCurrentMediaType failed");
    }

    UINT32 width = 0, height = 0;
    MFGetAttributeSize(actualType.Get(), MF_MT_FRAME_SIZE, &width, &height);
    m_width = static_cast<int32_t>(width);
    m_height = static_cast<int32_t>(height);

    // The MF buffer's row stride may include driver-specific padding, and per the MF
    // attribute's documented convention, a *negative* stride means the buffer is
    // bottom-up (last row first) rather than top-down. m_latestFrameData (what
    // TryGetLatestFrame exposes) is always top-down and tightly packed at width*4 —
    // both de-striding and any bottom-up flip happen once here in the capture thread
    // rather than being every consumer's problem (see CaptureThreadMain).
    if (FAILED(actualType->GetUINT32(MF_MT_DEFAULT_STRIDE, reinterpret_cast<UINT32*>(&m_sourceStrideBytes))) ||
        m_sourceStrideBytes == 0) {
        m_sourceStrideBytes = m_width * 4;
    }
    m_sourceIsBottomUp = m_sourceStrideBytes < 0;
    if (m_sourceIsBottomUp) {
        m_sourceStrideBytes = -m_sourceStrideBytes;
    }
    m_strideBytes = m_width * 4;

    hr = m_reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
    if (FAILED(hr)) {
        throw std::runtime_error("SetStreamSelection failed");
    }

    m_latestFrameData.resize(static_cast<size_t>(m_strideBytes) * static_cast<size_t>(m_height));

    m_captureThread = std::thread(&UvcCaptureSource::CaptureThreadMain, this);
}

UvcCaptureSource::~UvcCaptureSource() {
    m_stopRequested.store(true, std::memory_order_release);
    if (m_reader) {
        // Unblocks a ReadSample() that may be waiting on the driver; ignored if already stopped.
        m_reader->Flush(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM));
    }
    if (m_captureThread.joinable()) {
        m_captureThread.join();
    }
    if (m_mediaSource) {
        m_mediaSource->Shutdown();
    }
}

void UvcCaptureSource::CaptureThreadMain() {
    while (!m_stopRequested.load(std::memory_order_acquire)) {
        DWORD streamIndex = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;

        HRESULT hr = m_reader->ReadSample(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &streamIndex, &flags, &timestamp, &sample);

        if (m_stopRequested.load(std::memory_order_acquire)) {
            break;
        }
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
            break;
        }
        if (!sample) {
            continue; // gap in the stream; keep polling
        }

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
            continue;
        }

        BYTE* data = nullptr;
        DWORD dataLength = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &dataLength))) {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            const size_t packedRowBytes = static_cast<size_t>(m_strideBytes); // == width*4
            const size_t sourceStride = static_cast<size_t>(m_sourceStrideBytes);
            const bool needsRepack = sourceStride != packedRowBytes || m_sourceIsBottomUp;

            if (!needsRepack) {
                const size_t copyLength = std::min<size_t>(dataLength, m_latestFrameData.size());
                std::memcpy(m_latestFrameData.data(), data, copyLength);
            } else {
                // Strip driver row padding and/or flip bottom-up rows so the exposed
                // buffer is always top-down and tightly packed.
                const size_t maxRows = dataLength / sourceStride;
                const size_t rowsToCopy = std::min<size_t>(maxRows, static_cast<size_t>(m_height));
                for (size_t row = 0; row < rowsToCopy; ++row) {
                    const size_t sourceRow = m_sourceIsBottomUp ? (rowsToCopy - 1 - row) : row;
                    std::memcpy(
                        m_latestFrameData.data() + row * packedRowBytes,
                        data + sourceRow * sourceStride,
                        packedRowBytes);
                }
            }
            m_latestTimestamp100ns = timestamp;
            m_hasFrame = true;
        }

        buffer->Unlock();
    }
}

bool UvcCaptureSource::TryGetLatestFrame(CcFrameBuffer& frame) {
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
