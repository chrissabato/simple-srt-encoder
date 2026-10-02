#include "WasapiAudioCapture.h"
#include "../Diagnostics.h"

#include <objbase.h>
#include <propidl.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <audioclient.h>
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace capturecore {

namespace {

bool SubFormatEquals(const GUID& a, const GUID& b) {
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

// Only the two shared-mode mix formats Windows actually hands out in practice are
// supported: IEEE float 32-bit (the near-universal case since Vista's audio engine
// mixes internally in float) and PCM 16-bit (seen on some exclusive/legacy drivers).
// Anything else fails Open() cleanly rather than risk mismatched-format audio reaching
// ffmpeg silently.
bool ResolvePcmFormat(const WAVEFORMATEX* wfx, bool& outIsFloat) {
    static constexpr GUID kSubtypeIeeeFloat = {
        0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

    WORD formatTag = wfx->wFormatTag;
    if (formatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        formatTag = SubFormatEquals(ext->SubFormat, kSubtypeIeeeFloat) ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    }

    if (formatTag == WAVE_FORMAT_IEEE_FLOAT && wfx->wBitsPerSample == 32) {
        outIsFloat = true;
        return true;
    }
    if (formatTag == WAVE_FORMAT_PCM && wfx->wBitsPerSample == 16) {
        outIsFloat = false;
        return true;
    }
    return false;
}

} // namespace

std::vector<CcAudioDeviceInfo> EnumerateAudioCaptureDevices() {
    std::vector<CcAudioDeviceInfo> result;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        return result;
    }

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection))) {
        return result;
    }

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) {
            continue;
        }

        CcAudioDeviceInfo info{};

        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)) && id != nullptr) {
            wcsncpy_s(info.id.value, CC_MAX_STRING, id, _TRUNCATE);
            CoTaskMemFree(id);
        }

        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT nameProp;
            PropVariantInit(&nameProp);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &nameProp)) && nameProp.vt == VT_LPWSTR) {
                wcsncpy_s(info.displayName, CC_MAX_STRING, nameProp.pwszVal, _TRUNCATE);
            }
            PropVariantClear(&nameProp);
        }

        result.push_back(info);
    }

    return result;
}

WasapiAudioCapture::WasapiAudioCapture() = default;

WasapiAudioCapture::~WasapiAudioCapture() {
    Close();
}

bool WasapiAudioCapture::Open(const CcDeviceId& deviceId) {
    Close();

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        return false;
    }

    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDevice(deviceId.value, &device))) {
        LogDiagnostic(L"WasapiAudioCapture::Open: GetDevice failed for '" + std::wstring(deviceId.value) + L"'");
        return false;
    }

    if (FAILED(device->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(m_audioClient.GetAddressOf())))) {
        LogDiagnostic(L"WasapiAudioCapture::Open: IAudioClient activation failed");
        return false;
    }

    WAVEFORMATEX* mixFormat = nullptr;
    if (FAILED(m_audioClient->GetMixFormat(&mixFormat)) || mixFormat == nullptr) {
        Close();
        return false;
    }

    bool isFloat = false;
    if (!ResolvePcmFormat(mixFormat, isFloat)) {
        LogDiagnostic(L"WasapiAudioCapture::Open: unsupported mix format (tag=" +
                      std::to_wstring(mixFormat->wFormatTag) + L" bits=" +
                      std::to_wstring(mixFormat->wBitsPerSample) + L")");
        CoTaskMemFree(mixFormat);
        Close();
        return false;
    }

    m_sampleRate = static_cast<int32_t>(mixFormat->nSamplesPerSec);
    m_channels = static_cast<int32_t>(mixFormat->nChannels);
    m_isFloat = isFloat;

    // 200ms shared-mode buffer, event-driven (matches Microsoft's own WASAPI capture
    // samples): generous relative to the ~10ms packets Windows actually delivers, so a
    // briefly delayed capture-thread wakeup never overruns it.
    constexpr REFERENCE_TIME kBufferDuration = 200 * 10000; // 100ns units
    const HRESULT hr = m_audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kBufferDuration, 0, mixFormat, nullptr);
    CoTaskMemFree(mixFormat);
    if (FAILED(hr)) {
        LogDiagnostic(L"WasapiAudioCapture::Open: IAudioClient::Initialize failed, hr=0x" +
                      std::to_wstring(static_cast<uint32_t>(hr)));
        Close();
        return false;
    }

    m_captureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_captureEvent || FAILED(m_audioClient->SetEventHandle(m_captureEvent))) {
        Close();
        return false;
    }

    if (FAILED(m_audioClient->GetService(
            __uuidof(IAudioCaptureClient), reinterpret_cast<void**>(m_captureClient.GetAddressOf())))) {
        Close();
        return false;
    }

    return true;
}

void WasapiAudioCapture::Close() {
    StopStreaming();
    m_captureClient.Reset();
    m_audioClient.Reset();
    if (m_captureEvent) {
        CloseHandle(m_captureEvent);
        m_captureEvent = nullptr;
    }
    m_sampleRate = 0;
    m_channels = 0;
    m_isFloat = false;
}

bool WasapiAudioCapture::StartStreaming(FramePipeWriter& pipe) {
    if (!m_audioClient || !m_captureClient) {
        return false;
    }
    m_stopRequested.store(false);
    {
        std::lock_guard<std::mutex> lock(m_pipeQueueMutex);
        m_pipeQueue.clear();
        m_pipeQueueBytes = 0;
    }
    m_pipeWriterThread = std::thread(&WasapiAudioCapture::PipeWriterThreadMain, this, &pipe);
    m_thread = std::thread(&WasapiAudioCapture::CaptureThreadMain, this, &pipe, AudioSink{});
    return true;
}

bool WasapiAudioCapture::StartMonitoring(AudioSink sink) {
    if (!m_audioClient || !m_captureClient) {
        return false;
    }
    m_stopRequested.store(false);
    m_thread = std::thread(&WasapiAudioCapture::CaptureThreadMain, this, nullptr, std::move(sink));
    return true;
}

void WasapiAudioCapture::StopStreaming() {
    m_stopRequested.store(true);
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_pipeQueueCv.notify_all(); // wake the writer thread so it notices the stop request
    if (m_pipeWriterThread.joinable()) {
        m_pipeWriterThread.join();
    }
}

void WasapiAudioCapture::CaptureThreadMain(FramePipeWriter* pipe, AudioSink sink) {
    if (pipe) {
        LogDiagnostic(L"WasapiAudioCapture: capture thread started, waiting for pipe connection...");
        if (!pipe->WaitForConnection(5000)) {
            LogDiagnostic(L"WasapiAudioCapture: audio pipe connection timed out, exiting");
            return;
        }
        LogDiagnostic(L"WasapiAudioCapture: audio pipe connected, starting capture");
    }

    const UINT32 bytesPerFrame = static_cast<UINT32>(m_channels) * (m_isFloat ? 4 : 2);

    if (FAILED(m_audioClient->Start())) {
        LogDiagnostic(L"WasapiAudioCapture: IAudioClient::Start failed");
        return;
    }

    // Distinguishes "audio is queued but intact, just delayed behind a slow ffmpeg drain
    // (recoverable once it clears)" from "audio is being lost somewhere we're not logging"
    // — added 2026-10-02 after a field test showed real, growing A/V lag with zero
    // overflow-drop log lines, which this alone can't explain without seeing whether the
    // backlog itself was actually large.
    auto lastQueueDepthLogTime = std::chrono::steady_clock::now();

    while (!m_stopRequested.load()) {
        const DWORD waitResult = WaitForSingleObject(m_captureEvent, 200);
        if (m_stopRequested.load()) {
            break;
        }
        if (waitResult != WAIT_OBJECT_0) {
            continue; // timeout; loop back and re-check the stop flag
        }

        UINT32 packetLength = 0;
        if (FAILED(m_captureClient->GetNextPacketSize(&packetLength))) {
            break;
        }

        bool captureFailed = false;
        while (packetLength != 0) {
            BYTE* data = nullptr;
            UINT32 numFrames = 0;
            DWORD flags = 0;
            if (FAILED(m_captureClient->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr))) {
                captureFailed = true;
                break;
            }

            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                // WASAPI's own glitch signal: it detected a gap between this packet and the
                // last one (we didn't call GetBuffer/ReleaseBuffer often enough to keep up
                // with the endpoint's buffer, so it silently lost data before we ever saw
                // it). This happens upstream of m_pipeQueue entirely — the overflow-drop log
                // a few lines down can only catch loss *after* this point, so a gap here is
                // invisible there. Logged on its own so a real-but-unlogged loss (the
                // confirmed-in-the-field 2026-10-02 case: queue overflow fixed, but audio
                // still measurably behind with zero overflow-drop log lines) is diagnosable
                // instead of a dead end.
                LogDiagnostic(L"WasapiAudioCapture: WASAPI reported AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY "
                              L"(capture thread fell behind the endpoint; audio was lost before reaching "
                              L"our queue)");
            }

            const size_t byteCount = static_cast<size_t>(numFrames) * bytesPerFrame;
            std::vector<uint8_t> silence;
            const uint8_t* samples = data;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                // WASAPI docs: data may not point at valid silence when this flag is set;
                // use explicit zeros instead of whatever's actually in the buffer.
                silence.assign(byteCount, 0);
                samples = silence.data();
            }
            if (sink) {
                sink(samples, byteCount);
            }
            if (pipe) {
                // Fast, bounded, in-memory push only — never blocks on I/O. See
                // m_pipeQueue's declaration for why this must never wait on the pipe
                // directly here: doing so used to hold this WASAPI buffer segment
                // hostage to ffmpeg's read pace, and WASAPI silently drops audio once
                // its own (200ms) buffer fills while nobody's draining it.
                std::vector<uint8_t> chunk(samples, samples + byteCount);
                std::lock_guard<std::mutex> lock(m_pipeQueueMutex);
                // Confirmed in the field (2026-10-02 field log): a real ffmpeg-side stall
                // (encode throughput collapsing to ~17fps for ~2.5 minutes, cause outside
                // this process) drained this queue's old 4MB (~a few seconds) cap almost
                // immediately, then dropped audio continuously for the rest of the stall —
                // permanently deleting ~2.5 minutes of real audio content while the video
                // path (which never drops, only delays — see TryGetLatestFrame) kept every
                // frame. That asymmetry is precisely what produced a permanent, growing
                // A/V offset: video was late but complete, audio was on-time but missing
                // chunks. Sized generously now (minutes, not seconds) so a stall in this
                // range degrades into recoverable delay — matching video's existing
                // behavior — instead of unrecoverable desync; PCM is cheap enough that
                // buffering several minutes of it is trivial compared to the cost of
                // dropping any of it. Logged either way so a real overload (or a stall
                // longer than this) is still diagnosable instead of silent.
                constexpr size_t kMaxQueuedBytes = 128 * 1024 * 1024; // ~minutes of PCM
                m_pipeQueue.push_back(std::move(chunk));
                m_pipeQueueBytes += byteCount;
                if (m_pipeQueueBytes > kMaxQueuedBytes) {
                    LogDiagnostic(L"WasapiAudioCapture: pipe queue overflow, dropping oldest chunk "
                                  L"(ffmpeg sustained-stalling on the audio pipe)");
                    m_pipeQueueBytes -= m_pipeQueue.front().size();
                    m_pipeQueue.pop_front();
                }
                m_pipeQueueCv.notify_one();

                const auto now = std::chrono::steady_clock::now();
                if (now - lastQueueDepthLogTime >= std::chrono::seconds(5)) {
                    lastQueueDepthLogTime = now;
                    const double approxSeconds =
                        static_cast<double>(m_pipeQueueBytes) / (static_cast<double>(m_sampleRate) * bytesPerFrame);
                    LogDiagnostic(L"WasapiAudioCapture: pipe queue backlog ~" +
                                  std::to_wstring(static_cast<long long>(approxSeconds * 1000)) +
                                  L"ms (" + std::to_wstring(m_pipeQueueBytes) + L" bytes)");
                }
            }
            m_captureClient->ReleaseBuffer(numFrames);

            if (FAILED(m_captureClient->GetNextPacketSize(&packetLength))) {
                captureFailed = true;
                break;
            }
        }
        if (captureFailed) {
            LogDiagnostic(L"WasapiAudioCapture: WASAPI capture call failed, exiting capture loop");
            break;
        }
        if (pipe && m_stopRequested.load()) {
            break; // the writer thread hit a pipe failure and asked us to stop too
        }
    }

    m_audioClient->Stop();
    LogDiagnostic(L"WasapiAudioCapture: capture thread exiting");
}

void WasapiAudioCapture::PipeWriterThreadMain(FramePipeWriter* pipe) {
    LogDiagnostic(L"WasapiAudioCapture: pipe writer thread started");
    for (;;) {
        std::vector<uint8_t> chunk;
        {
            std::unique_lock<std::mutex> lock(m_pipeQueueMutex);
            m_pipeQueueCv.wait(lock, [this] { return !m_pipeQueue.empty() || m_stopRequested.load(); });
            if (m_pipeQueue.empty()) {
                // Only reachable via the stop request, since the predicate above only
                // wakes on non-empty-or-stop — drain whatever's left before actually
                // exiting (below) rather than discarding buffered audio on stop.
                if (m_stopRequested.load()) {
                    break;
                }
                continue;
            }
            chunk = std::move(m_pipeQueue.front());
            m_pipeQueue.pop_front();
            m_pipeQueueBytes -= chunk.size();
        }

        // Blocking here is fine and intended — it's exactly what this thread exists to
        // absorb, decoupled from the real-time WASAPI capture loop.
        if (!pipe->WriteFrame(chunk.data(), chunk.size())) {
            LogDiagnostic(L"WasapiAudioCapture: pipe write failed, stopping capture");
            m_stopRequested.store(true); // wake CaptureThreadMain too: ffmpeg exited
            break;
        }
    }
    LogDiagnostic(L"WasapiAudioCapture: pipe writer thread exiting");
}

} // namespace capturecore
