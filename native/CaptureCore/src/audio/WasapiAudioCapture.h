#pragma once

#include "capturecore/capture_types.h"
#include "../encode/FramePipeWriter.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

struct IAudioClient;
struct IAudioCaptureClient;

namespace capturecore {

// Enumerates active WASAPI capture endpoints (mics/line-in), independent of any video
// capture backend. Not wrapped in an ICaptureDeviceEnumerator: that interface's Open()
// returns an ICaptureSource (pull-model video frames), which doesn't fit a continuous
// audio stream — see WasapiAudioCapture below instead.
std::vector<CcAudioDeviceInfo> EnumerateAudioCaptureDevices();

// Captures PCM audio from one WASAPI endpoint in shared mode and streams it to ffmpeg
// over a dedicated named pipe, mirroring the video path's FramePipeWriter usage but with
// its own capture thread (audio has no preview/pull consumer to synchronize with, so it
// pushes continuously rather than being polled like ICaptureSource).
class WasapiAudioCapture {
public:
    WasapiAudioCapture();
    ~WasapiAudioCapture();

    WasapiAudioCapture(const WasapiAudioCapture&) = delete;
    WasapiAudioCapture& operator=(const WasapiAudioCapture&) = delete;

    // Activates the endpoint and negotiates its shared-mode mix format. Must succeed
    // before Start()/the format getters below are meaningful.
    bool Open(const CcDeviceId& deviceId);
    void Close();

    // Starts the capture thread: waits for ffmpeg to connect to pipe (same
    // fire-and-forget convention as CaptureManager::StreamingThreadMain — a connect
    // timeout just ends the thread quietly rather than reporting back to the caller),
    // then streams PCM chunks until StopStreaming(). The pipe must already exist (be
    // created before ffmpeg launches) and outlive the streaming session; the caller owns it.
    bool StartStreaming(FramePipeWriter& pipe);

    // Captures with no pipe, handing each PCM chunk (in the format reported by the
    // getters below) to sink on the capture thread. Used for level metering.
    using AudioSink = std::function<void(const uint8_t*, size_t)>;
    bool StartMonitoring(AudioSink sink);

    void StopStreaming();

    int32_t SampleRate() const { return m_sampleRate; }
    int32_t Channels() const { return m_channels; }
    bool IsFloatFormat() const { return m_isFloat; } // true => f32le, false => s16le

private:
    void CaptureThreadMain(FramePipeWriter* pipe, AudioSink sink);
    // Drains m_pipeQueue to pipe on its own thread so a slow/blocked ffmpeg can never
    // stall the WASAPI-facing capture loop above — see the long comment on
    // m_pipeQueue for why that matters.
    void PipeWriterThreadMain(FramePipeWriter* pipe);

    Microsoft::WRL::ComPtr<IAudioClient> m_audioClient;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> m_captureClient;
    HANDLE m_captureEvent = nullptr;

    int32_t m_sampleRate = 0;
    int32_t m_channels = 0;
    bool m_isFloat = false;

    std::thread m_thread;
    std::atomic<bool> m_stopRequested{false};

    // Real bug hit and fixed here: CaptureThreadMain used to call pipe->WriteFrame()
    // synchronously, blocking, *while still holding the WASAPI buffer segment between
    // GetBuffer/ReleaseBuffer*. If ffmpeg was briefly slow to drain the pipe (e.g. busy
    // with GPU encode), that block held up the next GetNextPacketSize/GetBuffer call —
    // meanwhile WASAPI kept capturing into its shared-mode buffer (200ms) in the
    // background with nobody draining it, and once that buffer filled, WASAPI silently
    // DROPPED audio. That's real, permanent data loss, not just delay — and it's why the
    // symptom was both "growing audio delay" (each drop means the remaining audio
    // represents less real duration than elapsed time, so it permanently falls further
    // behind) and "dropped audio packets" (audible gaps) from a single root cause. Video
    // can't hit this failure mode structurally (TryGetLatestFrame's "always return
    // latest" semantics can't overflow), but audio is a must-not-gap continuous stream,
    // so it needs an actual buffer here. This queue decouples the two: the capture loop
    // only ever does a fast, bounded, in-memory push (never blocks on I/O, so it always
    // gets back to GetBuffer/ReleaseBuffer in time), and this separate thread does the
    // (potentially blocking) pipe write independently.
    //
    // The bound itself (see kMaxQueuedBytes at the push site) still matters: this queue
    // only prevents the WASAPI-level drop above, not a drop of its own once IT fills. A
    // field log (2026-10-02) caught a real multi-minute ffmpeg-side stall that overflowed
    // the original few-seconds-sized cap and reproduced the exact same symptom one layer
    // up. The cap is now sized for minutes, not seconds, for that reason.
    std::mutex m_pipeQueueMutex;
    std::condition_variable m_pipeQueueCv;
    std::deque<std::vector<uint8_t>> m_pipeQueue;
    size_t m_pipeQueueBytes = 0;
    std::thread m_pipeWriterThread;
};

} // namespace capturecore
