#include "CaptureManager.h"
#include "uvc/UvcDeviceEnumerator.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mfapi.h>

namespace capturecore {

CaptureManager::CaptureManager() {
    m_mfInitialized = SUCCEEDED(MFStartup(MF_VERSION));
    m_enumerators.push_back(std::make_unique<UvcDeviceEnumerator>());
    // Phase 2/3: DeckLink/NDI enumerators are pushed here too, behind
    // CAPTURECORE_ENABLE_DECKLINK / CAPTURECORE_ENABLE_NDI (see CMakeLists.txt) —
    // no other code in this class needs to change when they're added.
}

CaptureManager::~CaptureManager() {
    CloseSource();
    if (m_mfInitialized) {
        MFShutdown();
    }
}

int32_t CaptureManager::EnumerateDevices(CcDeviceInfo* outArray, int32_t maxCount) {
    std::vector<CcDeviceInfo> all;
    for (auto& enumerator : m_enumerators) {
        if (!enumerator->IsAvailable()) {
            continue;
        }
        auto devices = enumerator->Enumerate();
        all.insert(all.end(), devices.begin(), devices.end());
    }

    const int32_t copyCount = std::min<int32_t>(maxCount, static_cast<int32_t>(all.size()));
    for (int32_t i = 0; i < copyCount; ++i) {
        outArray[i] = all[static_cast<size_t>(i)];
    }
    return static_cast<int32_t>(all.size());
}

bool CaptureManager::OpenSource(const CcDeviceId& id, const CcCaptureFormat& format) {
    CloseSource();

    for (auto& enumerator : m_enumerators) {
        if (!enumerator->IsAvailable()) {
            continue;
        }
        auto source = enumerator->Open(id, format);
        if (source) {
            std::lock_guard<std::mutex> lock(m_sourceMutex);
            m_activeSource = std::move(source);
            return true;
        }
    }
    return false;
}

void CaptureManager::CloseSource() {
    StopStream();
    std::lock_guard<std::mutex> lock(m_sourceMutex);
    m_activeSource.reset();
}

bool CaptureManager::GetOpenSourceSize(int32_t& width, int32_t& height) const {
    std::lock_guard<std::mutex> lock(m_sourceMutex);
    if (!m_activeSource) {
        return false;
    }
    width = m_activeSource->Width();
    height = m_activeSource->Height();
    return true;
}

bool CaptureManager::TryGetLatestFrame(CcFrameBuffer& frame) {
    std::lock_guard<std::mutex> lock(m_sourceMutex);
    if (!m_activeSource) {
        return false;
    }
    return m_activeSource->TryGetLatestFrame(frame);
}

bool CaptureManager::StartStream(const CcEncodeSettings& encode, const CcSrtSettings& srt) {
    if (IsStreaming()) {
        return false;
    }

    int32_t sourceWidth = 0, sourceHeight = 0;
    {
        std::lock_guard<std::mutex> lock(m_sourceMutex);
        if (!m_activeSource) {
            return false;
        }
        sourceWidth = m_activeSource->Width();
        sourceHeight = m_activeSource->Height();
    }

    const std::wstring pipeName =
        L"\\\\.\\pipe\\srtencoder_frames_" + std::to_wstring(GetCurrentProcessId());

    m_pipeWriter = std::make_unique<FramePipeWriter>(pipeName);
    if (!m_ffmpeg.Start(encode, srt, pipeName, sourceWidth, sourceHeight)) {
        m_pipeWriter.reset();
        return false;
    }

    m_streamingStopRequested.store(false);
    m_streamingThread = std::thread(&CaptureManager::StreamingThreadMain, this, encode, sourceWidth, sourceHeight);
    return true;
}

void CaptureManager::StopStream() {
    m_streamingStopRequested.store(true);
    m_ffmpeg.Stop();
    if (m_streamingThread.joinable()) {
        m_streamingThread.join();
    }
    m_pipeWriter.reset();
}

bool CaptureManager::IsStreaming() const {
    return m_ffmpeg.IsRunning();
}

CcStreamStats CaptureManager::GetStreamStats() const {
    return m_ffmpeg.GetStats();
}

void CaptureManager::StreamingThreadMain(CcEncodeSettings encode, int32_t sourceWidth, int32_t sourceHeight) {
    if (!m_pipeWriter->WaitForConnection(5000)) {
        // ffmpeg never connected (missing exe, crashed on startup, ...); GetStreamStats
        // will report Broken once the process exits, which FfmpegProcessController
        // detects independently.
        return;
    }

    // ICaptureSource always exposes tightly-packed BGRA32 (width*4 stride); any
    // driver row padding was already stripped by the capture backend (e.g.
    // UvcCaptureSource), so a single exactly-sized buffer is always sufficient here.
    std::vector<uint8_t> frameBuffer(static_cast<size_t>(sourceWidth) * sourceHeight * 4);

    const int32_t frameRateNum = std::max(1, encode.outputFrameRate.numerator);
    const int32_t frameRateDen = std::max(1, encode.outputFrameRate.denominator);
    const auto frameInterval = std::chrono::duration<double>(static_cast<double>(frameRateDen) / frameRateNum);
    auto nextFrameTime = std::chrono::steady_clock::now();

    while (!m_streamingStopRequested.load() && m_ffmpeg.IsRunning()) {
        CcFrameBuffer frame{};
        frame.data = frameBuffer.data();
        frame.capacity = static_cast<int32_t>(frameBuffer.size());

        if (TryGetLatestFrame(frame) && frame.width == sourceWidth && frame.height == sourceHeight) {
            if (!m_pipeWriter->WriteFrame(frameBuffer.data(), frameBuffer.size())) {
                break; // ffmpeg closed its end of the pipe (exited)
            }
        }

        nextFrameTime += std::chrono::duration_cast<std::chrono::steady_clock::duration>(frameInterval);
        std::this_thread::sleep_until(nextFrameTime);
    }
}

} // namespace capturecore
