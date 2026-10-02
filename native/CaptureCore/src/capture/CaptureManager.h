#pragma once

#include "ICaptureDeviceEnumerator.h"
#include "../audio/LoudnessMeter.h"
#include "../audio/WasapiAudioCapture.h"
#include "../encode/FfmpegProcessController.h"
#include "../encode/FramePipeWriter.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
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
    int32_t EnumerateAudioDevices(CcAudioDeviceInfo* outArray, int32_t maxCount);

    bool OpenSource(const CcDeviceId& id, const CcCaptureFormat& format);
    void CloseSource();
    bool TryGetLatestFrame(CcFrameBuffer& frame);
    bool GetOpenSourceSize(int32_t& width, int32_t& height) const;

    bool StartAudioMonitor(const CcDeviceId& id);
    void StopAudioMonitor();
    CcLoudness GetLoudness() { return m_loudness.GetReading(); }
    void ResetLoudness() { m_loudness.Reset(); }

    bool StartStream(const CcEncodeSettings& encode, const CcSrtSettings& srt);
    void StopStream();
    bool IsStreaming() const;
    CcStreamStats GetStreamStats() const;

    // Cached wrapper around FfmpegProcessController::ProbeEncoder — the probe itself
    // launches a real ffmpeg process, so this avoids re-running it (up to ~5s worst
    // case) every time the UI asks about the same (exeName, encoderName) pair, e.g.
    // re-resolving "auto" on every stream start within one session. exeName selects
    // which ffmpeg\<name>.exe to probe; empty means the default "ffmpeg.exe".
    bool ProbeEncoder(const std::wstring& encoderName, const std::wstring& exeName = L"");

private:
    void StreamingThreadMain(CcEncodeSettings encode, int32_t sourceWidth, int32_t sourceHeight);
    void EmbeddedAudioThreadMain();
    size_t ReadEmbeddedAudioFromSource(uint8_t* dest, size_t maxBytes);
    void SetSourceEmbeddedAudioCapture(bool enabled);

    bool m_comInitializedByUs = false;
    bool m_mfInitialized = false;
    std::vector<std::unique_ptr<ICaptureDeviceEnumerator>> m_enumerators;

    mutable std::mutex m_sourceMutex;
    std::unique_ptr<ICaptureSource> m_activeSource;

    std::unique_ptr<FramePipeWriter> m_pipeWriter;
    LoudnessMeter m_loudness;
    std::unique_ptr<WasapiAudioCapture> m_monitorCapture;

    std::unique_ptr<FramePipeWriter> m_audioPipeWriter;
    std::unique_ptr<WasapiAudioCapture> m_audioCapture;
    std::thread m_embeddedAudioThread;
    FfmpegProcessController m_ffmpeg;
    std::thread m_streamingThread;
    std::atomic<bool> m_streamingStopRequested{false};

    std::mutex m_encoderProbeMutex;
    std::unordered_map<std::wstring, bool> m_encoderProbeCache;
};

} // namespace capturecore
