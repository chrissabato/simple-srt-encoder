#include "CaptureManager.h"
#include "uvc/UvcDeviceEnumerator.h"
#include "directshow/DirectShowDeviceEnumerator.h"
#include "../Diagnostics.h"

#if defined(CAPTURECORE_ENABLE_DECKLINK)
#include "decklink/DeckLinkDeviceEnumerator.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <mfapi.h>
#include <objbase.h>

namespace capturecore {

CaptureManager::CaptureManager() {
    // Media Foundation calls made from this process's other threads (e.g. background
    // capture threads) generally don't need it, but DeckLink's CoCreateInstance calls
    // do — defensively ensure it here rather than assume the host (the C# UI thread)
    // already has it, since this DLL shouldn't assume anything about its caller's
    // threading setup. COINIT_APARTMENTTHREADED on an already-STA thread (which a
    // WinUI3 UI thread always is) just returns S_FALSE, not an error.
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    m_comInitializedByUs = (comResult == S_OK);

    m_mfInitialized = SUCCEEDED(MFStartup(MF_VERSION));
    m_enumerators.push_back(std::make_unique<UvcDeviceEnumerator>());
    m_enumerators.push_back(std::make_unique<DirectShowDeviceEnumerator>());
#if defined(CAPTURECORE_ENABLE_DECKLINK)
    m_enumerators.push_back(std::make_unique<DeckLinkDeviceEnumerator>());
#endif
    // Phase 3: the NDI enumerator is pushed here too, behind CAPTURECORE_ENABLE_NDI
    // (see CMakeLists.txt) — no other code in this class needs to change to add it.
}

CaptureManager::~CaptureManager() {
    StopAudioMonitor();
    CloseSource();
    if (m_mfInitialized) {
        MFShutdown();
    }
    if (m_comInitializedByUs) {
        CoUninitialize();
    }
}

int32_t CaptureManager::EnumerateDevices(CcDeviceInfo* outArray, int32_t maxCount) {
    std::vector<CcDeviceInfo> all;
    for (auto& enumerator : m_enumerators) {
        const int32_t backend = static_cast<int32_t>(enumerator->Backend());
        if (!enumerator->IsAvailable()) {
            LogDiagnostic(
                L"CaptureManager::EnumerateDevices: backend " + std::to_wstring(backend) +
                L" is not available, skipping");
            continue;
        }
        auto devices = enumerator->Enumerate();
        LogDiagnostic(
            L"CaptureManager::EnumerateDevices: backend " + std::to_wstring(backend) + L" available, found " +
            std::to_wstring(devices.size()) + L" device(s)");
        all.insert(all.end(), devices.begin(), devices.end());
    }

    const int32_t copyCount = std::min<int32_t>(maxCount, static_cast<int32_t>(all.size()));
    for (int32_t i = 0; i < copyCount; ++i) {
        outArray[i] = all[static_cast<size_t>(i)];
    }
    return static_cast<int32_t>(all.size());
}

int32_t CaptureManager::EnumerateAudioDevices(CcAudioDeviceInfo* outArray, int32_t maxCount) {
    std::vector<CcAudioDeviceInfo> devices;
#if defined(CAPTURECORE_ENABLE_DECKLINK)
    // Listed unconditionally (not only when a DeckLink source is open) since the UI
    // enumerates audio devices once at startup, before any source is opened; StartStream
    // falls back to video-only if the open source turns out to have no embedded audio.
    CcAudioDeviceInfo embedded{};
    wcsncpy_s(embedded.id.value, CC_MAX_STRING, CC_EMBEDDED_AUDIO_DEVICE_ID, _TRUNCATE);
    wcsncpy_s(embedded.displayName, CC_MAX_STRING, L"Embedded audio (capture source)", _TRUNCATE);
    devices.push_back(embedded);
#endif
    const std::vector<CcAudioDeviceInfo> wasapiDevices = EnumerateAudioCaptureDevices();
    devices.insert(devices.end(), wasapiDevices.begin(), wasapiDevices.end());

    const int32_t copyCount = std::min<int32_t>(maxCount, static_cast<int32_t>(devices.size()));
    for (int32_t i = 0; i < copyCount; ++i) {
        outArray[i] = devices[static_cast<size_t>(i)];
    }
    return static_cast<int32_t>(devices.size());
}

bool CaptureManager::StartAudioMonitor(const CcDeviceId& id) {
    StopAudioMonitor();

    if (wcscmp(id.value, CC_EMBEDDED_AUDIO_DEVICE_ID) == 0) {
        // Embedded audio has a single consumer (the streaming pump), so it's only
        // metered while streaming; just make sure the meter expects its fixed format.
        m_loudness.Configure(ICaptureSource::kEmbeddedAudioSampleRate, ICaptureSource::kEmbeddedAudioChannels);
        return true;
    }

    auto capture = std::make_unique<WasapiAudioCapture>();
    if (!capture->Open(id)) {
        return false;
    }
    m_loudness.Configure(capture->SampleRate(), capture->Channels());
    const bool isFloat = capture->IsFloatFormat();
    if (!capture->StartMonitoring([this, isFloat](const uint8_t* data, size_t bytes) {
            m_loudness.ProcessPcm(data, bytes, isFloat);
        })) {
        return false;
    }
    m_monitorCapture = std::move(capture);
    return true;
}

void CaptureManager::StopAudioMonitor() {
    m_monitorCapture.reset();
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

    // Mirrors the guard in FfmpegProcessController::Start(): if ffmpeg died on its own
    // during a previous session (IsStreaming() above only reflects m_ffmpeg, not these
    // threads) and StopStream() was never called to join them, m_streamingThread /
    // m_embeddedAudioThread are still joinable() even though their functions already
    // returned. Reassigning a joinable std::thread below is undefined behavior
    // (std::terminate()) — join any leftover thread first so this is always safe.
    if (m_streamingThread.joinable()) {
        m_streamingThread.join();
    }
    if (m_embeddedAudioThread.joinable()) {
        m_embeddedAudioThread.join();
    }
    // Same leftover-session hazard for WASAPI audio: a live m_audioCapture from a broken
    // session still references the old m_audioPipeWriter, which is about to be replaced
    // below.
    if (m_audioCapture) {
        m_audioCapture->StopStreaming();
        m_audioCapture.reset();
    }
    // Real bug hit and fixed here: `m_pipeWriter = std::make_unique<FramePipeWriter>(...)`
    // below evaluates the NEW FramePipeWriter's constructor (which calls
    // CreateNamedPipeW, nMaxInstances=1) BEFORE the move-assignment destroys the OLD one
    // — so if a previous session's pipe writer is still alive (ffmpeg died on its own,
    // StopStream() never called), CreateNamedPipeW for the same name fails silently
    // (FramePipeWriter doesn't check its own handle), leaving a dead writer that fails
    // WaitForConnection() near-instantly instead of actually waiting. Explicitly reset
    // both pipe writers here so the old OS pipe handle is always closed before a new one
    // is ever created.
    m_pipeWriter.reset();
    m_audioPipeWriter.reset();

    int32_t sourceWidth = 0, sourceHeight = 0;
    {
        std::lock_guard<std::mutex> lock(m_sourceMutex);
        if (!m_activeSource) {
            return false;
        }
        sourceWidth = m_activeSource->Width();
        sourceHeight = m_activeSource->Height();
    }

    const std::wstring pid = std::to_wstring(GetCurrentProcessId());
    const std::wstring pipeName = L"\\\\.\\pipe\\srtencoder_frames_" + pid;

    m_pipeWriter = std::make_unique<FramePipeWriter>(pipeName);

    AudioPipeConfig audioConfig;
    bool useEmbeddedAudio = false;
    if (encode.audioEnabled) {
        if (wcscmp(encode.audioDeviceId.value, CC_EMBEDDED_AUDIO_DEVICE_ID) == 0) {
            std::lock_guard<std::mutex> lock(m_sourceMutex);
            if (m_activeSource && m_activeSource->SupportsEmbeddedAudio()) {
                useEmbeddedAudio = true;
                audioConfig.enabled = true;
                audioConfig.sampleRate = ICaptureSource::kEmbeddedAudioSampleRate;
                audioConfig.channels = ICaptureSource::kEmbeddedAudioChannels;
                audioConfig.isFloat = false; // s16le
            } else {
                LogDiagnostic(L"StartStream: open source has no embedded audio; continuing video-only");
            }
        } else {
            auto audioCapture = std::make_unique<WasapiAudioCapture>();
            if (audioCapture->Open(encode.audioDeviceId)) {
                audioConfig.enabled = true;
                audioConfig.sampleRate = audioCapture->SampleRate();
                audioConfig.channels = audioCapture->Channels();
                audioConfig.isFloat = audioCapture->IsFloatFormat();
                m_audioCapture = std::move(audioCapture);
            } else {
                LogDiagnostic(L"StartStream: failed to open audio capture device; continuing video-only");
            }
        }
    }

    if (useEmbeddedAudio) {
        m_loudness.Configure(ICaptureSource::kEmbeddedAudioSampleRate, ICaptureSource::kEmbeddedAudioChannels);
    } else {
        m_loudness.Reset(); // integrated loudness starts fresh with each stream
    }

    if (audioConfig.enabled) {
        // Created before ffmpeg launches (like the video pipe) so ffmpeg's open of this
        // input can never race the pipe's existence.
        audioConfig.pipeName = L"\\\\.\\pipe\\srtencoder_audio_" + pid;
        m_audioPipeWriter = std::make_unique<FramePipeWriter>(audioConfig.pipeName);
    }

    if (!m_ffmpeg.Start(encode, srt, pipeName, sourceWidth, sourceHeight, audioConfig)) {
        m_pipeWriter.reset();
        m_audioPipeWriter.reset();
        m_audioCapture.reset();
        return false;
    }

    // Fire-and-forget, same convention as the video streaming thread below: a pipe
    // connect failure inside an audio thread just ends that thread quietly rather than
    // failing StartStream (ffmpeg was already launched expecting this input, so at this
    // point a video-only fallback would require restarting ffmpeg).
    m_streamingStopRequested.store(false);
    if (m_audioCapture) {
        m_audioCapture->StartStreaming(*m_audioPipeWriter);
    } else if (useEmbeddedAudio) {
        SetSourceEmbeddedAudioCapture(true);
        m_embeddedAudioThread = std::thread(&CaptureManager::EmbeddedAudioThreadMain, this);
    }

    m_streamingThread =std::thread(&CaptureManager::StreamingThreadMain, this, encode, sourceWidth, sourceHeight);
    return true;
}

void CaptureManager::StopStream() {
    LogDiagnostic(L"CaptureManager::StopStream() called");
    m_streamingStopRequested.store(true);
    LogDiagnostic(L"  calling m_ffmpeg.Stop()...");
    m_ffmpeg.Stop();
    LogDiagnostic(L"  m_ffmpeg.Stop() returned; joining streaming thread...");
    if (m_streamingThread.joinable()) {
        m_streamingThread.join();
    }
    LogDiagnostic(L"  streaming thread joined; stopping audio capture...");
    if (m_audioCapture) {
        m_audioCapture->StopStreaming();
        m_audioCapture.reset();
    }
    if (m_embeddedAudioThread.joinable()) {
        m_embeddedAudioThread.join();
        SetSourceEmbeddedAudioCapture(false);
    }
    LogDiagnostic(L"  resetting pipe writers...");
    m_audioPipeWriter.reset();
    m_pipeWriter.reset();
    LogDiagnostic(L"CaptureManager::StopStream() done");
}

bool CaptureManager::IsStreaming() const {
    return m_ffmpeg.IsRunning();
}

CcStreamStats CaptureManager::GetStreamStats() const {
    return m_ffmpeg.GetStats();
}

bool CaptureManager::ProbeEncoder(const std::wstring& encoderName, const std::wstring& exeName) {
    const std::wstring cacheKey = (exeName.empty() ? L"ffmpeg.exe" : exeName) + L"|" + encoderName;
    {
        std::lock_guard<std::mutex> lock(m_encoderProbeMutex);
        const auto it = m_encoderProbeCache.find(cacheKey);
        if (it != m_encoderProbeCache.end()) {
            return it->second;
        }
    }

    const bool available = FfmpegProcessController::ProbeEncoder(encoderName, exeName);
    LogDiagnostic(L"ProbeEncoder: " + cacheKey + L" -> " +
                  std::wstring(available ? L"available" : L"unavailable"));

    std::lock_guard<std::mutex> lock(m_encoderProbeMutex);
    m_encoderProbeCache[cacheKey] = available;
    return available;
}

size_t CaptureManager::ReadEmbeddedAudioFromSource(uint8_t* dest, size_t maxBytes) {
    std::lock_guard<std::mutex> lock(m_sourceMutex);
    return m_activeSource ? m_activeSource->ReadEmbeddedAudio(dest, maxBytes) : 0;
}

void CaptureManager::SetSourceEmbeddedAudioCapture(bool enabled) {
    std::lock_guard<std::mutex> lock(m_sourceMutex);
    if (m_activeSource) {
        m_activeSource->SetEmbeddedAudioCapture(enabled);
    }
}

void CaptureManager::EmbeddedAudioThreadMain() {
    LogDiagnostic(L"EmbeddedAudioThreadMain: started, waiting for audio pipe connection...");
    if (!m_audioPipeWriter->WaitForConnection(5000)) {
        LogDiagnostic(L"EmbeddedAudioThreadMain: audio pipe connection timed out, exiting");
        return;
    }
    LogDiagnostic(L"EmbeddedAudioThreadMain: audio pipe connected, pumping embedded audio");

    // 50ms of 48kHz s16le stereo; DeckLink delivers a packet per video frame (~16-33ms),
    // so this drains everything buffered since the last pass in a single write.
    std::vector<uint8_t> chunk(static_cast<size_t>(ICaptureSource::kEmbeddedAudioSampleRate) *
                               ICaptureSource::kEmbeddedAudioChannels * 2 / 20);

    while (!m_streamingStopRequested.load() && m_ffmpeg.IsRunning()) {
        const size_t bytes = ReadEmbeddedAudioFromSource(chunk.data(), chunk.size());
        if (bytes == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        m_loudness.ProcessPcm(chunk.data(), bytes, /*isFloat=*/false);
        if (!m_audioPipeWriter->WriteFrame(chunk.data(), bytes)) {
            LogDiagnostic(L"EmbeddedAudioThreadMain: WriteFrame failed, exiting loop");
            break;
        }
    }
    LogDiagnostic(L"EmbeddedAudioThreadMain: exiting");
}

void CaptureManager::StreamingThreadMain(CcEncodeSettings encode, int32_t sourceWidth, int32_t sourceHeight) {
    LogDiagnostic(L"StreamingThreadMain: started, waiting for pipe connection...");
    if (!m_pipeWriter->WaitForConnection(5000)) {
        // ffmpeg never connected (missing exe, crashed on startup, ...); GetStreamStats
        // will report Broken once the process exits, which FfmpegProcessController
        // detects independently.
        LogDiagnostic(L"StreamingThreadMain: pipe connection timed out, exiting");
        return;
    }
    LogDiagnostic(L"StreamingThreadMain: pipe connected, entering frame loop");

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
                LogDiagnostic(L"StreamingThreadMain: WriteFrame failed, exiting loop");
                break; // ffmpeg closed its end of the pipe (exited)
            }
        }

        nextFrameTime += std::chrono::duration_cast<std::chrono::steady_clock::duration>(frameInterval);
        std::this_thread::sleep_until(nextFrameTime);
    }
    LogDiagnostic(L"StreamingThreadMain: exiting (stopRequested=" +
                  std::wstring(m_streamingStopRequested.load() ? L"yes" : L"no") + L" ffmpegRunning=" +
                  std::wstring(m_ffmpeg.IsRunning() ? L"yes" : L"no") + L")");
}

} // namespace capturecore
