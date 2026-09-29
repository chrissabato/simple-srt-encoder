#include "UvcCaptureSource.h"
#include "../../Diagnostics.h"

#include <mfapi.h>
#include <mferror.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace capturecore {

namespace {

// Most video subtype GUIDs are FOURCC-based (Data1 holds the 4-char code, e.g. 'NV12',
// 'YUY2', 'MJPG', 'RGB4'); fall back to hex for the handful that aren't (RGB32 is
// D3DFMT_X8R8G8B8's GUID, not a FOURCC, so it won't decode as ASCII here).
std::wstring FormatSubtypeGuid(const GUID& guid) {
    wchar_t fourcc[5] = {
        static_cast<wchar_t>(guid.Data1 & 0xFF),
        static_cast<wchar_t>((guid.Data1 >> 8) & 0xFF),
        static_cast<wchar_t>((guid.Data1 >> 16) & 0xFF),
        static_cast<wchar_t>((guid.Data1 >> 24) & 0xFF),
        L'\0',
    };
    bool printable = true;
    for (int i = 0; i < 4; ++i) {
        if (fourcc[i] < 0x20 || fourcc[i] > 0x7E) {
            printable = false;
            break;
        }
    }
    if (printable) {
        return fourcc;
    }
    std::wstringstream ss;
    ss << std::hex << guid.Data1;
    return L"{0x" + ss.str() + L"...}";
}

std::wstring HResultToHex(HRESULT hr) {
    std::wstringstream ss;
    ss << L"0x" << std::hex << static_cast<unsigned long>(hr);
    return ss.str();
}

// Picks the native media type on the video stream that best matches the requested
// frame size AND is a subtype UvcCaptureSource can decode itself (YUY2/NV12/RGB32),
// falling back to a size-only match and finally to whatever's first if neither exists
// (in which case the constructor will reject it with a clear error rather than crash).
ComPtr<IMFMediaType> PickNativeType(IMFSourceReader* reader, const CcCaptureFormat& requested) {
    ComPtr<IMFMediaType> first;
    ComPtr<IMFMediaType> sizeMatch;
    ComPtr<IMFMediaType> sizeAndFormatMatch;

    for (DWORD typeIndex = 0;; ++typeIndex) {
        ComPtr<IMFMediaType> candidate;
        HRESULT hr = reader->GetNativeMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), typeIndex, &candidate);
        if (hr == MF_E_NO_MORE_TYPES || FAILED(hr)) {
            break;
        }

        UINT32 width = 0, height = 0;
        MFGetAttributeSize(candidate.Get(), MF_MT_FRAME_SIZE, &width, &height);
        GUID subtype = GUID_NULL;
        candidate->GetGUID(MF_MT_SUBTYPE, &subtype);
        LogDiagnostic(
            L"    native type[" + std::to_wstring(typeIndex) + L"]: " + std::to_wstring(width) + L"x" +
            std::to_wstring(height) + L" " + FormatSubtypeGuid(subtype));

        if (!first) {
            first = candidate;
        }

        const bool sizeMatches = requested.width > 0 && requested.height > 0 &&
            static_cast<int32_t>(width) == requested.width && static_cast<int32_t>(height) == requested.height;
        const bool formatSupported =
            subtype == MFVideoFormat_YUY2 || subtype == MFVideoFormat_NV12 || subtype == MFVideoFormat_RGB32;

        if (sizeMatches && !sizeMatch) {
            sizeMatch = candidate;
        }
        if (sizeMatches && formatSupported && !sizeAndFormatMatch) {
            sizeAndFormatMatch = candidate;
        }
    }

    if (sizeAndFormatMatch) {
        return sizeAndFormatMatch;
    }
    if (sizeMatch) {
        return sizeMatch;
    }
    if (first) {
        return first;
    }
    throw std::runtime_error("UVC device exposes no native media types");
}

uint8_t ClampToByte(int value) {
    return static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
}

// Standard ITU-R BT.601 YUV -> RGB conversion (integer approximation).
void YuvToBgr(int y, int u, int v, uint8_t& b, uint8_t& g, uint8_t& r) {
    const int c = y - 16;
    const int d = u - 128;
    const int e = v - 128;
    r = ClampToByte((298 * c + 409 * e + 128) >> 8);
    g = ClampToByte((298 * c - 100 * d - 208 * e + 128) >> 8);
    b = ClampToByte((298 * c + 516 * d + 128) >> 8);
}

// YUY2 is packed 4:2:2 — 4 bytes encode 2 horizontal pixels: Y0 U Y1 V.
void ConvertYuy2RowToBgra32(const uint8_t* srcRow, uint8_t* dstRow, int32_t width) {
    for (int32_t x = 0; x + 1 < width; x += 2) {
        const uint8_t* px = srcRow + static_cast<size_t>(x) * 2;
        const int y0 = px[0], u = px[1], y1 = px[2], v = px[3];

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

// NV12 is planar 4:2:0 — a full-resolution Y plane followed by a half-resolution plane
// with interleaved U,V samples, each covering a 2x2 block of luma pixels.
void ConvertNv12ToBgra32(
    const uint8_t* yPlane, size_t yStride, const uint8_t* uvPlane, size_t uvStride,
    uint8_t* dst, size_t dstStride, int32_t width, int32_t height) {
    for (int32_t row = 0; row < height; ++row) {
        const uint8_t* ySrc = yPlane + static_cast<size_t>(row) * yStride;
        const uint8_t* uvSrc = uvPlane + static_cast<size_t>(row / 2) * uvStride;
        uint8_t* dstRow = dst + static_cast<size_t>(row) * dstStride;

        for (int32_t x = 0; x < width; ++x) {
            const int y = ySrc[x];
            const int u = uvSrc[(x / 2) * 2 + 0];
            const int v = uvSrc[(x / 2) * 2 + 1];

            uint8_t b, g, r;
            YuvToBgr(y, u, v, b, g, r);
            uint8_t* px = dstRow + static_cast<size_t>(x) * 4;
            px[0] = b;
            px[1] = g;
            px[2] = r;
            px[3] = 0xFF;
        }
    }
}

} // namespace

UvcCaptureSource::UvcCaptureSource(ComPtr<IMFMediaSource> mediaSource, const CcCaptureFormat& requestedFormat)
    : m_mediaSource(std::move(mediaSource)) {
    LogDiagnostic(L"  UvcCaptureSource: creating source reader...");
    HRESULT hr = MFCreateSourceReaderFromMediaSource(m_mediaSource.Get(), nullptr, &m_reader);
    if (FAILED(hr)) {
        LogDiagnostic(L"  MFCreateSourceReaderFromMediaSource FAILED hr=" + HResultToHex(hr));
        throw std::runtime_error("MFCreateSourceReaderFromMediaSource failed");
    }
    LogDiagnostic(L"  source reader created; enumerating native types (requested " +
                  std::to_wstring(requestedFormat.width) + L"x" + std::to_wstring(requestedFormat.height) + L")...");

    ComPtr<IMFMediaType> nativeType = PickNativeType(m_reader.Get(), requestedFormat);

    GUID subtype = GUID_NULL;
    nativeType->GetGUID(MF_MT_SUBTYPE, &subtype);
    if (subtype == MFVideoFormat_YUY2) {
        m_sourceFormat = UvcSourceFormat::Yuy2;
    } else if (subtype == MFVideoFormat_NV12) {
        m_sourceFormat = UvcSourceFormat::Nv12;
    } else if (subtype == MFVideoFormat_RGB32) {
        m_sourceFormat = UvcSourceFormat::Bgra32;
    } else {
        LogDiagnostic(L"  unsupported native subtype: " + FormatSubtypeGuid(subtype));
        throw std::runtime_error("UVC device's native pixel format is not supported (only YUY2/NV12/RGB32)");
    }

    // Locks the native type exactly as the device reported it — no MF-side format
    // conversion is requested (see the class comment in the header for why).
    LogDiagnostic(L"  locking native type...");
    hr = m_reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, nativeType.Get());
    if (FAILED(hr)) {
        LogDiagnostic(L"  SetCurrentMediaType(native) FAILED hr=" + HResultToHex(hr));
        throw std::runtime_error("SetCurrentMediaType(native) failed");
    }

    UINT32 width = 0, height = 0;
    MFGetAttributeSize(nativeType.Get(), MF_MT_FRAME_SIZE, &width, &height);
    m_width = static_cast<int32_t>(width);
    m_height = static_cast<int32_t>(height);
    LogDiagnostic(L"  locked format: " + std::to_wstring(m_width) + L"x" + std::to_wstring(m_height));

    // MF_MT_DEFAULT_STRIDE is the primary plane's stride (the only plane for YUY2/RGB32,
    // the Y plane for NV12 — its UV plane is assumed to share the same stride, which
    // holds for every real NV12 UVC device encountered so far). A negative stride means
    // a bottom-up buffer; that convention is RGB-specific in practice; YUY2/NV12 aren't
    // flipped here.
    INT32 stride = 0;
    if (FAILED(nativeType->GetUINT32(MF_MT_DEFAULT_STRIDE, reinterpret_cast<UINT32*>(&stride))) || stride == 0) {
        switch (m_sourceFormat) {
            case UvcSourceFormat::Yuy2: stride = m_width * 2; break;
            case UvcSourceFormat::Nv12: stride = m_width; break;
            case UvcSourceFormat::Bgra32: stride = m_width * 4; break;
        }
    }
    m_sourceIsBottomUp = m_sourceFormat == UvcSourceFormat::Bgra32 && stride < 0;
    m_sourceStrideBytes = m_sourceIsBottomUp ? -stride : stride;
    m_strideBytes = m_width * 4; // m_latestFrameData is always tightly-packed BGRA32

    hr = m_reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
    if (FAILED(hr)) {
        throw std::runtime_error("SetStreamSelection failed");
    }

    m_latestFrameData.resize(static_cast<size_t>(m_strideBytes) * static_cast<size_t>(m_height));

    m_captureThread = std::thread(&UvcCaptureSource::CaptureThreadMain, this);

    // Wait briefly for the first frame (or a fault) so OpenSource() reports failure for
    // a device that negotiates a format successfully but can't actually stream it
    // (observed for real on at least one device: MF_E_HW_MFT_FAILED_START_STREAMING on
    // the very first ReadSample) instead of silently "succeeding" into a dead source.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool gotFrame = false;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            gotFrame = m_hasFrame;
        }
        if (gotFrame || m_readerFaulted.load(std::memory_order_acquire)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (!gotFrame) {
        LogDiagnostic(L"  no frame within timeout (faulted=" +
                      std::wstring(m_readerFaulted.load() ? L"yes" : L"no") + L"); failing Open");
        // The constructor is throwing, so its own destructor will never run — clean up
        // the thread/media source manually rather than leaking them.
        m_stopRequested.store(true, std::memory_order_release);
        if (m_reader && !m_readerFaulted.load(std::memory_order_acquire)) {
            m_reader->Flush(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM));
        }
        if (m_captureThread.joinable()) {
            m_captureThread.join();
        }
        if (m_mediaSource) {
            m_mediaSource->Shutdown();
        }
        throw std::runtime_error("Device did not produce a frame (failed to start streaming)");
    }
}

UvcCaptureSource::~UvcCaptureSource() {
    m_stopRequested.store(true, std::memory_order_release);
    // Per IMFSourceReader's documented contract, once ReadSample has reported an error
    // no further calls to the reader are safe — Flush() included. Skipping it here is
    // correct, not just defensive: CaptureThreadMain has already exited its loop in
    // that case, so there's nothing for Flush() to unblock anyway.
    if (m_reader && !m_readerFaulted.load(std::memory_order_acquire)) {
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
    LogDiagnostic(L"  CaptureThreadMain: started");
    int loggedIterations = 0;

    while (!m_stopRequested.load(std::memory_order_acquire)) {
        DWORD streamIndex = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;

        HRESULT hr = m_reader->ReadSample(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &streamIndex, &flags, &timestamp, &sample);

        if (loggedIterations < 5) {
            LogDiagnostic(
                L"  CaptureThreadMain: ReadSample hr=" + HResultToHex(hr) + L" flags=0x" +
                std::to_wstring(flags) + L" hasSample=" + (sample ? L"yes" : L"no"));
            ++loggedIterations;
        }

        if (m_stopRequested.load(std::memory_order_acquire)) {
            break;
        }
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
            LogDiagnostic(L"  CaptureThreadMain: exiting loop (reader faulted)");
            m_readerFaulted.store(true, std::memory_order_release);
            break;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            LogDiagnostic(L"  CaptureThreadMain: exiting loop (end of stream)");
            break;
        }
        if (!sample) {
            continue; // gap in the stream; keep polling
        }

        ComPtr<IMFMediaBuffer> buffer;
        const HRESULT convertHr = sample->ConvertToContiguousBuffer(&buffer);
        if (FAILED(convertHr)) {
            if (loggedIterations <= 5) {
                LogDiagnostic(L"  CaptureThreadMain: ConvertToContiguousBuffer FAILED hr=" + HResultToHex(convertHr));
            }
            continue;
        }

        BYTE* data = nullptr;
        DWORD dataLength = 0;
        const HRESULT lockHr = buffer->Lock(&data, nullptr, &dataLength);
        if (FAILED(lockHr)) {
            if (loggedIterations <= 5) {
                LogDiagnostic(L"  CaptureThreadMain: Lock FAILED hr=" + HResultToHex(lockHr));
            }
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            const size_t packedRowBytes = static_cast<size_t>(m_strideBytes); // == width*4
            const size_t sourceStride = static_cast<size_t>(m_sourceStrideBytes);

            switch (m_sourceFormat) {
                case UvcSourceFormat::Yuy2: {
                    const size_t maxRows = dataLength / sourceStride;
                    const size_t rowsToConvert = std::min<size_t>(maxRows, static_cast<size_t>(m_height));
                    for (size_t row = 0; row < rowsToConvert; ++row) {
                        ConvertYuy2RowToBgra32(
                            data + row * sourceStride, m_latestFrameData.data() + row * packedRowBytes, m_width);
                    }
                    break;
                }
                case UvcSourceFormat::Nv12: {
                    const uint8_t* yPlane = data;
                    const uint8_t* uvPlane = data + sourceStride * static_cast<size_t>(m_height);
                    const size_t uvPlaneBytes = dataLength > sourceStride * static_cast<size_t>(m_height)
                        ? dataLength - sourceStride * static_cast<size_t>(m_height)
                        : 0;
                    const size_t maxUvRows = sourceStride > 0 ? uvPlaneBytes / sourceStride : 0;
                    const size_t usableHeight =
                        std::min<size_t>(static_cast<size_t>(m_height), maxUvRows * 2);
                    if (usableHeight > 0) {
                        ConvertNv12ToBgra32(
                            yPlane, sourceStride, uvPlane, sourceStride, m_latestFrameData.data(), packedRowBytes,
                            m_width, static_cast<int32_t>(usableHeight));
                    }
                    break;
                }
                case UvcSourceFormat::Bgra32: {
                    if (sourceStride == packedRowBytes && !m_sourceIsBottomUp) {
                        const size_t copyLength = std::min<size_t>(dataLength, m_latestFrameData.size());
                        std::memcpy(m_latestFrameData.data(), data, copyLength);
                    } else {
                        const size_t maxRows = dataLength / sourceStride;
                        const size_t rowsToCopy = std::min<size_t>(maxRows, static_cast<size_t>(m_height));
                        for (size_t row = 0; row < rowsToCopy; ++row) {
                            const size_t sourceRow = m_sourceIsBottomUp ? (rowsToCopy - 1 - row) : row;
                            std::memcpy(
                                m_latestFrameData.data() + row * packedRowBytes, data + sourceRow * sourceStride,
                                packedRowBytes);
                        }
                    }
                    break;
                }
            }

            m_latestTimestamp100ns = timestamp;
            const bool wasFirstFrame = !m_hasFrame;
            m_hasFrame = true;
            if (wasFirstFrame) {
                LogDiagnostic(L"  CaptureThreadMain: first frame captured, dataLength=" + std::to_wstring(dataLength));
            }
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
