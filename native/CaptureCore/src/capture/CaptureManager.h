#pragma once

#include "ICaptureDeviceEnumerator.h"
#include "../encode/FfmpegProcessController.h"
#include "../encode/FramePipeWriter.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace capturecore {

// Owns device enumeration, the single active capture source, and the encode/SRT
// streaming pipeline. One instance backs one CaptureCoreHandle (see CaptureCoreApi.cpp).
class CaptureManager {
public:
    CaptureManager();
    ~CaptureManager();

    CaptureManager(const CaptureManager&) = delete;
    CaptureManager& operator=(const CaptureManager&) = delete;

    int32_t EnumerateDevices(CcDeviceInfo* outArray, int32_t maxCount);

    bool OpenSource(const CcDeviceId& id, const CcCaptureFormat& format);
    void CloseSource();
    bool TryGetLatestFrame(CcFrameBuffer& frame);
    bool GetOpenSourceSize(int32_t& width, int32_t& height) const;

    bool StartStream(const CcEncodeSettings& encode, const CcSrtSettings& srt);
    void StopStream();
    bool IsStreaming() const;
    CcStreamStats GetStreamStats() const;

private:
    void StreamingThreadMain(CcEncodeSettings encode, int32_t sourceWidth, int32_t sourceHeight);

    bool m_mfInitialized = false;
    std::vector<std::unique_ptr<ICaptureDeviceEnumerator>> m_enumerators;

    mutable std::mutex m_sourceMutex;
    std::unique_ptr<ICaptureSource> m_activeSource;

    std::unique_ptr<FramePipeWriter> m_pipeWriter;
    FfmpegProcessController m_ffmpeg;
    std::thread m_streamingThread;
    std::atomic<bool> m_streamingStopRequested{false};
};

} // namespace capturecore
