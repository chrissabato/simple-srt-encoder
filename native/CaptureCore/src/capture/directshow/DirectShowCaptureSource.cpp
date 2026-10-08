#include "DirectShowCaptureSource.h"
#include "../../Diagnostics.h"

#include <algorithm>
#include <amvideo.h>
#include <chrono>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace capturecore {

namespace {

std::wstring HResultToHex(HRESULT hr) {
    std::wstringstream ss;
    ss << L"0x" << std::hex << static_cast<unsigned long>(hr);
    return ss.str();
}

// Most video subtype GUIDs are FOURCC-based (Data1 holds the 4-char code, e.g. 'NV12',
// 'YUY2'); fall back to hex for the handful that aren't (RGB24/RGB32 use a fixed
// Data1, not a FOURCC). Same helper as UvcCaptureSource.cpp's copy — kept local to
// each backend rather than shared, see that file's equivalent for why.
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

uint8_t ClampToByte(int value) {
    return static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
}

// Same ITU-R BT.601 math as the scalar-multiply version this replaced (and as
// UvcCaptureSource.cpp's still-scalar copy — see that file for the constants'
// derivation), just precomputed: a field test (2026-10-02) measured the multiply
// version's per-frame cost at ~31ms average against a 33.3ms budget at 1080p30 — on its
// own enough to cap real throughput around 24-26fps, confirmed with NVENC idle (15%) at
// the same time, so the cost was here, not downstream. Each term below depends on only
// one input byte, so it's exactly 256 precomputed values per term; summing precomputed
// terms instead of multiplying per pixel removes the only expensive part of the
// original math while producing bit-identical output.
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

// YUY2 is packed 4:2:2 — 4 bytes encode 2 horizontal pixels: Y0 U Y1 V.
//
// Takes the lookup tables by parameter and computes the four chroma-dependent terms
// (uToB/uToG/vToR/vToG) once per pixel pair instead of once per luma sample — U/V are
// shared by both samples in 4:2:2 — same algebraic simplification already proven
// bit-identical for DeckLinkCaptureSource's equivalent UYVY converter (2026-10-07).
void ConvertYuy2RowToBgra32(const YuvToBgrTables& t, const uint8_t* __restrict srcRow, uint8_t* __restrict dstRow, int32_t width) {
    for (int32_t x = 0; x + 1 < width; x += 2) {
        const uint8_t* px = srcRow + static_cast<size_t>(x) * 2;
        const int y0 = px[0], u = px[1], y1 = px[2], v = px[3];

        const int uToB = t.uToB[u];
        const int uToG = t.uToG[u];
        const int vToR = t.vToR[v];
        const int vToG = t.vToG[v];

        const int yTerm0 = t.y[y0];
        uint8_t* dst0 = dstRow + static_cast<size_t>(x) * 4;
        dst0[0] = ClampToByte((yTerm0 + uToB + 128) >> 8);
        dst0[1] = ClampToByte((yTerm0 + uToG + vToG + 128) >> 8);
        dst0[2] = ClampToByte((yTerm0 + vToR + 128) >> 8);
        dst0[3] = 0xFF;

        const int yTerm1 = t.y[y1];
        uint8_t* dst1 = dstRow + static_cast<size_t>(x + 1) * 4;
        dst1[0] = ClampToByte((yTerm1 + uToB + 128) >> 8);
        dst1[1] = ClampToByte((yTerm1 + uToG + vToG + 128) >> 8);
        dst1[2] = ClampToByte((yTerm1 + vToR + 128) >> 8);
        dst1[3] = 0xFF;
    }
}

// UYVY is packed 4:2:2 like YUY2, just with U/Y/V/Y byte order instead of Y/U/Y/V —
// the traditional broadcast/SDI packing (vMix's virtual outputs negotiate this one).
// Same per-pixel-pair chroma-term simplification as ConvertYuy2RowToBgra32 above.
void ConvertUyvyRowToBgra32(const YuvToBgrTables& t, const uint8_t* __restrict srcRow, uint8_t* __restrict dstRow, int32_t width) {
    for (int32_t x = 0; x + 1 < width; x += 2) {
        const uint8_t* px = srcRow + static_cast<size_t>(x) * 2;
        const int u = px[0], y0 = px[1], v = px[2], y1 = px[3];

        const int uToB = t.uToB[u];
        const int uToG = t.uToG[u];
        const int vToR = t.vToR[v];
        const int vToG = t.vToG[v];

        const int yTerm0 = t.y[y0];
        uint8_t* dst0 = dstRow + static_cast<size_t>(x) * 4;
        dst0[0] = ClampToByte((yTerm0 + uToB + 128) >> 8);
        dst0[1] = ClampToByte((yTerm0 + uToG + vToG + 128) >> 8);
        dst0[2] = ClampToByte((yTerm0 + vToR + 128) >> 8);
        dst0[3] = 0xFF;

        const int yTerm1 = t.y[y1];
        uint8_t* dst1 = dstRow + static_cast<size_t>(x + 1) * 4;
        dst1[0] = ClampToByte((yTerm1 + uToB + 128) >> 8);
        dst1[1] = ClampToByte((yTerm1 + uToG + vToG + 128) >> 8);
        dst1[2] = ClampToByte((yTerm1 + vToR + 128) >> 8);
        dst1[3] = 0xFF;
    }
}

// Splits a packed-YUV frame's row conversion across worker threads — ported from
// DeckLinkCaptureSource's identical fix (2026-10-07) after direct timing on this
// backend (DirectShowCaptureSource::OnBuffer's lockWait/conversion diagnostic) found
// the same single-core-pegged bottleneck: conversion averaging ~18-20ms but spiking to
// 35-46ms against a 33.3ms budget at 30fps, causing the pacing thread to occasionally
// re-sample a stale frame (visible as stutter on the receiving end) despite ffmpeg's
// own progress stats staying clean throughout. Row-by-row conversion has zero
// cross-row dependencies, so this is a safe, purely-parallelized speedup (identical
// per-pixel math, just computed concurrently) rather than a SIMD rewrite. Kept at a
// conservative fixed 4 workers, same reasoning as DeckLink's copy, to avoid
// oversubscribing a machine also running ffmpeg/NVENC during a real stream.
template <typename RowConverter>
void ConvertRowsParallel(
    RowConverter&& convertRow, const uint8_t* src, size_t sourceStride, uint8_t* dst, size_t packedRowBytes,
    int32_t width, size_t rowsToConvert) {
    const YuvToBgrTables& table = GetYuvToBgrTables();
    constexpr size_t kMaxConversionWorkers = 4;
    const size_t workerCount = (std::min)(kMaxConversionWorkers, std::max<size_t>(1, rowsToConvert));
    const size_t rowsPerWorker = (rowsToConvert + workerCount - 1) / workerCount;
    auto convertRows = [&](size_t startRow, size_t endRow) {
        for (size_t row = startRow; row < endRow; ++row) {
            convertRow(table, src + row * sourceStride, dst + row * packedRowBytes, width);
        }
    };
    std::vector<std::thread> workers;
    for (size_t w = 1; w < workerCount; ++w) {
        const size_t startRow = w * rowsPerWorker;
        const size_t endRow = (std::min)(rowsToConvert, startRow + rowsPerWorker);
        if (startRow >= endRow) {
            break;
        }
        workers.emplace_back(convertRows, startRow, endRow);
    }
    // This thread (the DirectShow grabber's own callback thread) does the first band
    // itself rather than spawning a 4th worker just to then sit idle waiting on it.
    convertRows(0, (std::min)(rowsToConvert, rowsPerWorker));
    for (auto& worker : workers) {
        worker.join();
    }
}

void FreeMediaType(AM_MEDIA_TYPE& mt) {
    if (mt.cbFormat != 0 && mt.pbFormat != nullptr) {
        CoTaskMemFree(mt.pbFormat);
        mt.cbFormat = 0;
        mt.pbFormat = nullptr;
    }
    if (mt.pUnk != nullptr) {
        mt.pUnk->Release();
        mt.pUnk = nullptr;
    }
}

} // namespace

// Minimal ISampleGrabberCB implementation. AddRef/Release are deliberately no-ops
// (return 1): this object's lifetime is owned outright by the enclosing
// DirectShowCaptureSource, not by COM reference counting, which is the standard
// pattern for a local sample-grabber callback — the destructor calls
// ISampleGrabber::SetCallback(nullptr, 0) to unregister it from the graph before the
// object is destroyed, so BufferCB can never fire on a dangling owner pointer.
class DirectShowCaptureSource::GrabberCallback : public ISampleGrabberCB {
public:
    explicit GrabberCallback(DirectShowCaptureSource& owner) : m_owner(owner) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == __uuidof(ISampleGrabberCB)) {
            *ppv = static_cast<ISampleGrabberCB*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }

    STDMETHODIMP SampleCB(double, IMediaSample*) override { return E_NOTIMPL; }
    STDMETHODIMP BufferCB(double, BYTE* buffer, long bufferLen) override {
        m_owner.OnBuffer(buffer, bufferLen);
        return S_OK;
    }

private:
    DirectShowCaptureSource& m_owner;
};

DirectShowCaptureSource::DirectShowCaptureSource(
    ComPtr<IBaseFilter> sourceFilter, const CcCaptureFormat& /*requestedFormat*/)
    : m_sourceFilter(std::move(sourceFilter)) {
    // Unlike UvcCaptureSource, the requested width/height isn't used to pick among
    // several native types — DirectShow software filters generally expose exactly
    // one output format, so the graph just takes whatever RenderStream negotiates and
    // this constructor adapts to it (queried below via GetConnectedMediaType).

    LogDiagnostic(L"DirectShowCaptureSource: building filter graph...");
    HRESULT hr = CoCreateInstance(CLSID_DShow_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_graph));
    if (FAILED(hr)) {
        throw std::runtime_error("DirectShow: CoCreateInstance(CLSID_FilterGraph) failed");
    }
    hr = CoCreateInstance(
        CLSID_DShow_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_graphBuilder));
    if (FAILED(hr)) {
        throw std::runtime_error("DirectShow: CoCreateInstance(CLSID_CaptureGraphBuilder2) failed");
    }
    m_graphBuilder->SetFiltergraph(m_graph.Get());

    hr = m_graph->AddFilter(m_sourceFilter.Get(), L"Video Capture Source");
    if (FAILED(hr)) {
        throw std::runtime_error("DirectShow: AddFilter(source) failed");
    }

    ComPtr<IBaseFilter> grabberBase;
    hr = CoCreateInstance(CLSID_DShow_SampleGrabber, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&grabberBase));
    if (FAILED(hr)) {
        LogDiagnostic(L"  CoCreateInstance(CLSID_SampleGrabber) FAILED hr=" + HResultToHex(hr));
        throw std::runtime_error("DirectShow: Sample Grabber (qedit) is not available on this system");
    }
    m_grabberFilter = grabberBase;
    hr = grabberBase.As(&m_grabber);
    if (FAILED(hr)) {
        throw std::runtime_error("DirectShow: QueryInterface(ISampleGrabber) failed");
    }
    hr = m_graph->AddFilter(m_grabberFilter.Get(), L"Sample Grabber");
    if (FAILED(hr)) {
        throw std::runtime_error("DirectShow: AddFilter(grabber) failed");
    }

    hr = CoCreateInstance(CLSID_DShow_NullRenderer, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_nullRenderer));
    if (FAILED(hr)) {
        throw std::runtime_error("DirectShow: CoCreateInstance(CLSID_NullRenderer) failed");
    }
    hr = m_graph->AddFilter(m_nullRenderer.Get(), L"Null Renderer");
    if (FAILED(hr)) {
        throw std::runtime_error("DirectShow: AddFilter(null renderer) failed");
    }

    m_grabber->SetBufferSamples(FALSE); // pushed via the BufferCB callback, not pulled
    m_grabber->SetOneShot(FALSE);

    LogDiagnostic(L"  connecting source -> grabber -> null renderer...");
    hr = m_graphBuilder->RenderStream(
        &PIN_CATEGORY_DShow_Capture, &MEDIATYPE_DShow_Video, m_sourceFilter.Get(), m_grabberFilter.Get(),
        m_nullRenderer.Get());
    if (FAILED(hr)) {
        LogDiagnostic(L"  RenderStream FAILED hr=" + HResultToHex(hr));
        throw std::runtime_error(
            "DirectShow: RenderStream failed (device may already be in use, or exposes no connectable video pin)");
    }

    AM_MEDIA_TYPE mt{};
    hr = m_grabber->GetConnectedMediaType(&mt);
    if (FAILED(hr) || mt.formattype != FORMAT_DShow_VideoInfo || mt.pbFormat == nullptr) {
        LogDiagnostic(L"  GetConnectedMediaType FAILED or unrecognized format hr=" + HResultToHex(hr));
        FreeMediaType(mt);
        throw std::runtime_error("DirectShow: GetConnectedMediaType failed or format unrecognized");
    }

    const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(mt.pbFormat);
    m_width = vih->bmiHeader.biWidth;
    const LONG biHeight = vih->bmiHeader.biHeight;
    m_height = std::abs(static_cast<int>(biHeight));

    if (mt.subtype == MEDIASUBTYPE_DShow_RGB24) {
        m_sourceFormat = SourceFormat::Rgb24;
        m_sourceIsBottomUp = biHeight > 0;
    } else if (mt.subtype == MEDIASUBTYPE_DShow_RGB32) {
        m_sourceFormat = SourceFormat::Rgb32;
        m_sourceIsBottomUp = biHeight > 0;
    } else if (mt.subtype == MEDIASUBTYPE_DShow_YUY2) {
        m_sourceFormat = SourceFormat::Yuy2;
        m_sourceIsBottomUp = false; // packed YUV formats aren't flipped in DirectShow
    } else if (mt.subtype == MEDIASUBTYPE_DShow_UYVY) {
        m_sourceFormat = SourceFormat::Uyvy;
        m_sourceIsBottomUp = false;
    } else {
        LogDiagnostic(L"  unsupported negotiated subtype: " + FormatSubtypeGuid(mt.subtype));
        FreeMediaType(mt);
        throw std::runtime_error(
            "DirectShow: device's negotiated pixel format is not supported (only RGB24/RGB32/YUY2/UYVY)");
    }
    FreeMediaType(mt);
    const wchar_t* formatName = L"?";
    switch (m_sourceFormat) {
        case SourceFormat::Rgb24: formatName = L"RGB24 (row copy, cheap)"; break;
        case SourceFormat::Rgb32: formatName = L"RGB32 (row copy, cheap)"; break;
        case SourceFormat::Yuy2: formatName = L"YUY2 (per-pixel CPU conversion)"; break;
        case SourceFormat::Uyvy: formatName = L"UYVY (per-pixel CPU conversion)"; break;
    }
    // Format matters here specifically because YUY2/UYVY go through OnBuffer's per-pixel
    // YuvToBgr path below (~2M calls/frame at 1080p) instead of a straight row memcpy —
    // real, measurable CPU cost on the DirectShow callback thread that has nothing to do
    // with ffmpeg or NVENC, and invisible to both the GPU encode engine and casual overall
    // CPU% glances (easily hidden on a single core of a multi-core machine). Logged so a
    // throughput problem traced to this path doesn't need re-deriving the format first.
    LogDiagnostic(L"  negotiated format: " + std::to_wstring(m_width) + L"x" + std::to_wstring(m_height) +
                  L" " + std::wstring(formatName));

    m_strideBytes = m_width * 4; // m_latestFrameData is always tightly-packed BGRA32
    m_latestFrameData.resize(static_cast<size_t>(m_strideBytes) * static_cast<size_t>(m_height));

    m_callback = std::make_unique<GrabberCallback>(*this);
    m_grabber->SetCallback(m_callback.get(), 1); // 1 = BufferCB (raw bytes, not IMediaSample)

    hr = m_graph.As(&m_mediaControl);
    if (FAILED(hr)) {
        throw std::runtime_error("DirectShow: QueryInterface(IMediaControl) failed");
    }
    hr = m_mediaControl->Run();
    if (FAILED(hr)) {
        LogDiagnostic(L"  IMediaControl::Run FAILED hr=" + HResultToHex(hr));
        throw std::runtime_error("DirectShow: IMediaControl::Run failed");
    }

    // Wait briefly for the first frame so Open() reports failure for a device that
    // connects successfully but never actually delivers a sample, instead of silently
    // "succeeding" into a dead source — same rationale as UvcCaptureSource's ctor.
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
        LogDiagnostic(L"  no frame within timeout; failing Open");
        m_grabber->SetCallback(nullptr, 0);
        m_mediaControl->Stop();
        throw std::runtime_error("DirectShow: device did not produce a frame (failed to start streaming)");
    }
}

DirectShowCaptureSource::~DirectShowCaptureSource() {
    if (m_grabber) {
        m_grabber->SetCallback(nullptr, 0);
    }
    if (m_mediaControl) {
        m_mediaControl->Stop();
    }
}

void DirectShowCaptureSource::OnBuffer(const uint8_t* data, long length) {
    if (length <= 0) {
        return;
    }

    const auto waitStart = std::chrono::steady_clock::now();

    // Split from std::lock_guard so lock-WAIT time (contention with TryGetLatestFrame's
    // memcpy on the pacing thread) can be measured separately from the conversion work
    // itself — added 2026-10-02 because replacing the scalar YUV math with lookup tables
    // (a real, isolated speedup for the arithmetic alone) didn't move the total OnBuffer
    // time at all, which means the arithmetic was never the dominant cost and this lock
    // is the next, more likely, suspect.
    std::unique_lock<std::mutex> lock(m_frameMutex);
    const auto conversionStart = std::chrono::steady_clock::now();
    const size_t packedRowBytes = static_cast<size_t>(m_strideBytes);

    switch (m_sourceFormat) {
        case SourceFormat::Rgb24: {
            const size_t sourceStride = static_cast<size_t>(m_width) * 3;
            const size_t maxRows = sourceStride > 0 ? static_cast<size_t>(length) / sourceStride : 0;
            const size_t rowsToCopy = std::min<size_t>(maxRows, static_cast<size_t>(m_height));
            for (size_t row = 0; row < rowsToCopy; ++row) {
                const size_t sourceRow = m_sourceIsBottomUp ? (rowsToCopy - 1 - row) : row;
                const uint8_t* src = data + sourceRow * sourceStride;
                uint8_t* dst = m_latestFrameData.data() + row * packedRowBytes;
                for (int32_t x = 0; x < m_width; ++x) {
                    dst[x * 4 + 0] = src[x * 3 + 0];
                    dst[x * 4 + 1] = src[x * 3 + 1];
                    dst[x * 4 + 2] = src[x * 3 + 2];
                    dst[x * 4 + 3] = 0xFF;
                }
            }
            break;
        }
        case SourceFormat::Rgb32: {
            const size_t sourceStride = static_cast<size_t>(m_width) * 4;
            const size_t maxRows = sourceStride > 0 ? static_cast<size_t>(length) / sourceStride : 0;
            const size_t rowsToCopy = std::min<size_t>(maxRows, static_cast<size_t>(m_height));
            for (size_t row = 0; row < rowsToCopy; ++row) {
                const size_t sourceRow = m_sourceIsBottomUp ? (rowsToCopy - 1 - row) : row;
                uint8_t* dst = m_latestFrameData.data() + row * packedRowBytes;
                std::memcpy(dst, data + sourceRow * sourceStride, packedRowBytes);
                // RGB32's 4th byte is undefined, same caveat as the Media Foundation
                // path (see MainPage.xaml.cs) — force it opaque.
                for (int32_t x = 0; x < m_width; ++x) {
                    dst[x * 4 + 3] = 0xFF;
                }
            }
            break;
        }
        case SourceFormat::Yuy2: {
            const size_t sourceStride = static_cast<size_t>(m_width) * 2;
            const size_t maxRows = sourceStride > 0 ? static_cast<size_t>(length) / sourceStride : 0;
            const size_t rowsToConvert = std::min<size_t>(maxRows, static_cast<size_t>(m_height));
            ConvertRowsParallel(
                ConvertYuy2RowToBgra32, data, sourceStride, m_latestFrameData.data(), packedRowBytes, m_width,
                rowsToConvert);
            break;
        }
        case SourceFormat::Uyvy: {
            const size_t sourceStride = static_cast<size_t>(m_width) * 2;
            const size_t maxRows = sourceStride > 0 ? static_cast<size_t>(length) / sourceStride : 0;
            const size_t rowsToConvert = std::min<size_t>(maxRows, static_cast<size_t>(m_height));
            ConvertRowsParallel(
                ConvertUyvyRowToBgra32, data, sourceStride, m_latestFrameData.data(), packedRowBytes, m_width,
                rowsToConvert);
            break;
        }
    }

    m_hasFrame = true;

    // Lock-wait (contention with TryGetLatestFrame's memcpy on the pacing thread) and
    // conversion time tracked separately — added 2026-10-02 after the lookup-table
    // rewrite of the YUV math (a real, isolated speedup for the arithmetic alone) didn't
    // move the previously-combined total at all, meaning the arithmetic was never the
    // dominant cost. This splits the two so the real culprit shows up as a hard number
    // instead of another guess.
    const auto conversionEnd = std::chrono::steady_clock::now();
    const auto lockWait = conversionStart - waitStart;
    const auto conversion = conversionEnd - conversionStart;
    m_totalLockWaitTimeSinceLog += std::chrono::duration_cast<std::chrono::nanoseconds>(lockWait);
    m_maxLockWaitTimeSinceLog = std::max(
        m_maxLockWaitTimeSinceLog, std::chrono::duration_cast<std::chrono::nanoseconds>(lockWait));
    m_totalConversionTimeSinceLog += std::chrono::duration_cast<std::chrono::nanoseconds>(conversion);
    m_maxConversionTimeSinceLog = std::max(
        m_maxConversionTimeSinceLog, std::chrono::duration_cast<std::chrono::nanoseconds>(conversion));
    ++m_conversionCountSinceLog;
    if (conversionEnd - m_lastConversionLogTime >= std::chrono::seconds(5)) {
        m_lastConversionLogTime = conversionEnd;
        const auto n = static_cast<double>(m_conversionCountSinceLog);
        const double convAvgMs = (static_cast<double>(m_totalConversionTimeSinceLog.count()) / 1'000'000.0) / n;
        const double convMaxMs = static_cast<double>(m_maxConversionTimeSinceLog.count()) / 1'000'000.0;
        const double waitAvgMs = (static_cast<double>(m_totalLockWaitTimeSinceLog.count()) / 1'000'000.0) / n;
        const double waitMaxMs = static_cast<double>(m_maxLockWaitTimeSinceLog.count()) / 1'000'000.0;
        LogDiagnostic(L"DirectShowCaptureSource: lockWait avg=" + std::to_wstring(waitAvgMs) + L"ms max=" +
                      std::to_wstring(waitMaxMs) + L"ms, conversion avg=" + std::to_wstring(convAvgMs) +
                      L"ms max=" + std::to_wstring(convMaxMs) + L"ms, over " +
                      std::to_wstring(m_conversionCountSinceLog) + L" frames (33.3ms budget at 30fps)");
        m_maxConversionTimeSinceLog = std::chrono::nanoseconds{0};
        m_totalConversionTimeSinceLog = std::chrono::nanoseconds{0};
        m_maxLockWaitTimeSinceLog = std::chrono::nanoseconds{0};
        m_totalLockWaitTimeSinceLog = std::chrono::nanoseconds{0};
        m_conversionCountSinceLog = 0;
    }
}

bool DirectShowCaptureSource::TryGetLatestFrame(CcFrameBuffer& frame) {
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
    frame.timestamp100ns = 0; // not consumed downstream (see UvcCaptureSource's equivalent field)
    return true;
}

} // namespace capturecore
