#pragma once

#include "capturecore/capture_types.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace capturecore {

// ITU-R BS.1770 / EBU R128 loudness meter: K-weighting filter, 100ms sub-blocks,
// momentary (400ms), short-term (3s) and gated integrated loudness, plus sample peak.
// All channels are weighted equally (correct for mono/stereo; surround channel weights
// aren't applied). Process*() is called from an audio thread while GetReading()/Reset()
// are called from other threads — everything is guarded by one mutex.
class LoudnessMeter {
public:
    static constexpr double kFloorLufs = -120.0;

    // Sets the input format and clears all history. Must be called before Process*().
    void Configure(int32_t sampleRate, int32_t channels);
    void Reset();

    // Interleaved PCM, either float32 or signed 16-bit little-endian.
    void ProcessPcm(const uint8_t* data, size_t byteCount, bool isFloat);

    CcLoudness GetReading();

private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    };
    struct ChannelState {
        double s1x1 = 0, s1x2 = 0, s1y1 = 0, s1y2 = 0; // stage 1 (shelf) history
        double s2x1 = 0, s2x2 = 0, s2y1 = 0, s2y2 = 0; // stage 2 (high-pass) history
        double blockSumSquares = 0;
    };

    void ResetLocked();
    void ProcessFloatLocked(const float* interleaved, size_t frames);
    void FinishSubBlockLocked();
    double IntegratedLufsLocked();
    static double PowerToLufs(double power);

    std::mutex m_mutex;
    int32_t m_sampleRate = 0;
    int32_t m_channels = 0;
    int32_t m_subBlockFrames = 0;
    int32_t m_subBlockFill = 0;
    Biquad m_shelf;
    Biquad m_highPass;
    std::vector<ChannelState> m_state;
    double m_subBlockPeak = 0;

    std::deque<double> m_recentPowers; // last 30 sub-block powers (3s)
    std::deque<double> m_recentPeaks;  // matching per-sub-block peaks
    std::vector<double> m_gatingBlockPowers; // 400ms blocks, one per sub-block once warmed up
    bool m_integratedDirty = false;
    double m_cachedIntegrated = kFloorLufs;
};

} // namespace capturecore
