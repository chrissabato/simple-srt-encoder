#include "LoudnessMeter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace capturecore {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr size_t kShortTermSubBlocks = 30; // 3s of 100ms sub-blocks
constexpr size_t kMomentarySubBlocks = 4;  // 400ms
constexpr size_t kMaxGatingBlocks = 1'000'000;

// BS.1770 K-weighting filter coefficients for an arbitrary sample rate (same derivation
// as libebur128, since the standard only tabulates 48kHz).
void ComputeKWeighting(double fs, double& s1b0, double& s1b1, double& s1b2, double& s1a1, double& s1a2,
                       double& s2a1, double& s2a2) {
    {
        const double f0 = 1681.974450955533;
        const double G = 3.999843853973347;
        const double Q = 0.7071752369554196;
        const double K = std::tan(kPi * f0 / fs);
        const double Vh = std::pow(10.0, G / 20.0);
        const double Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        s1b0 = (Vh + Vb * K / Q + K * K) / a0;
        s1b1 = 2.0 * (K * K - Vh) / a0;
        s1b2 = (Vh - Vb * K / Q + K * K) / a0;
        s1a1 = 2.0 * (K * K - 1.0) / a0;
        s1a2 = (1.0 - K / Q + K * K) / a0;
    }
    {
        const double f0 = 38.13547087602444;
        const double Q = 0.5003270373238773;
        const double K = std::tan(kPi * f0 / fs);
        const double a0 = 1.0 + K / Q + K * K;
        s2a1 = 2.0 * (K * K - 1.0) / a0;
        s2a2 = (1.0 - K / Q + K * K) / a0;
    }
}

double Mean(const std::deque<double>& values, size_t lastN) {
    if (values.empty()) {
        return 0.0;
    }
    const size_t n = std::min(lastN, values.size());
    double sum = 0.0;
    for (size_t i = values.size() - n; i < values.size(); ++i) {
        sum += values[i];
    }
    return sum / static_cast<double>(n);
}

} // namespace

double LoudnessMeter::PowerToLufs(double power) {
    if (power <= 0.0) {
        return kFloorLufs;
    }
    return std::max(kFloorLufs, -0.691 + 10.0 * std::log10(power));
}

void LoudnessMeter::Configure(int32_t sampleRate, int32_t channels) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sampleRate = std::max(1, sampleRate);
    m_channels = std::max(1, channels);
    m_subBlockFrames = std::max(1, m_sampleRate / 10);

    double s2a1 = 0, s2a2 = 0;
    ComputeKWeighting(m_sampleRate, m_shelf.b0, m_shelf.b1, m_shelf.b2, m_shelf.a1, m_shelf.a2, s2a1, s2a2);
    m_highPass = Biquad{1.0, -2.0, 1.0, s2a1, s2a2};

    ResetLocked();
}

void LoudnessMeter::Reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    ResetLocked();
}

void LoudnessMeter::ResetLocked() {
    m_state.assign(static_cast<size_t>(std::max(1, m_channels)), ChannelState{});
    m_subBlockFill = 0;
    m_subBlockPeak = 0;
    m_recentPowers.clear();
    m_recentPeaks.clear();
    m_gatingBlockPowers.clear();
    m_integratedDirty = false;
    m_cachedIntegrated = kFloorLufs;
}

void LoudnessMeter::ProcessPcm(const uint8_t* data, size_t byteCount, bool isFloat) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_channels <= 0 || m_subBlockFrames <= 0) {
        return;
    }
    const size_t bytesPerFrame = static_cast<size_t>(m_channels) * (isFloat ? 4 : 2);
    const size_t frames = byteCount / bytesPerFrame;
    if (frames == 0) {
        return;
    }

    if (isFloat) {
        std::vector<float> samples(frames * static_cast<size_t>(m_channels));
        std::memcpy(samples.data(), data, samples.size() * sizeof(float));
        ProcessFloatLocked(samples.data(), frames);
    } else {
        std::vector<float> samples(frames * static_cast<size_t>(m_channels));
        for (size_t i = 0; i < samples.size(); ++i) {
            int16_t s;
            std::memcpy(&s, data + i * 2, sizeof(s));
            samples[i] = static_cast<float>(s) / 32768.0f;
        }
        ProcessFloatLocked(samples.data(), frames);
    }
}

void LoudnessMeter::ProcessFloatLocked(const float* interleaved, size_t frames) {
    const size_t channels = static_cast<size_t>(m_channels);
    for (size_t f = 0; f < frames; ++f) {
        for (size_t c = 0; c < channels; ++c) {
            const double x = interleaved[f * channels + c];
            m_subBlockPeak = std::max(m_subBlockPeak, std::fabs(x));

            ChannelState& st = m_state[c];
            const double y1 = m_shelf.b0 * x + m_shelf.b1 * st.s1x1 + m_shelf.b2 * st.s1x2 -
                              m_shelf.a1 * st.s1y1 - m_shelf.a2 * st.s1y2;
            st.s1x2 = st.s1x1;
            st.s1x1 = x;
            st.s1y2 = st.s1y1;
            st.s1y1 = y1;

            const double y2 = m_highPass.b0 * y1 + m_highPass.b1 * st.s2x1 + m_highPass.b2 * st.s2x2 -
                              m_highPass.a1 * st.s2y1 - m_highPass.a2 * st.s2y2;
            st.s2x2 = st.s2x1;
            st.s2x1 = y1;
            st.s2y2 = st.s2y1;
            st.s2y1 = y2;

            st.blockSumSquares += y2 * y2;
        }
        if (++m_subBlockFill >= m_subBlockFrames) {
            FinishSubBlockLocked();
        }
    }
}

void LoudnessMeter::FinishSubBlockLocked() {
    double power = 0.0;
    for (ChannelState& st : m_state) {
        power += st.blockSumSquares / static_cast<double>(m_subBlockFrames);
        st.blockSumSquares = 0.0;
    }

    m_recentPowers.push_back(power);
    m_recentPeaks.push_back(m_subBlockPeak);
    if (m_recentPowers.size() > kShortTermSubBlocks) {
        m_recentPowers.pop_front();
        m_recentPeaks.pop_front();
    }
    m_subBlockFill = 0;
    m_subBlockPeak = 0.0;

    // A gating block is 400ms with 75% overlap, i.e. one per 100ms sub-block once four exist.
    if (m_recentPowers.size() >= kMomentarySubBlocks) {
        if (m_gatingBlockPowers.size() >= kMaxGatingBlocks) {
            m_gatingBlockPowers.erase(m_gatingBlockPowers.begin(), m_gatingBlockPowers.begin() + kMaxGatingBlocks / 2);
        }
        m_gatingBlockPowers.push_back(Mean(m_recentPowers, kMomentarySubBlocks));
        m_integratedDirty = true;
    }
}

double LoudnessMeter::IntegratedLufsLocked() {
    if (!m_integratedDirty) {
        return m_cachedIntegrated;
    }
    m_integratedDirty = false;

    // Absolute gate at -70 LUFS, then a relative gate 10 LU below the ungated-by-relative mean.
    const double absoluteGatePower = std::pow(10.0, (-70.0 + 0.691) / 10.0);
    double sum = 0.0;
    size_t count = 0;
    for (const double p : m_gatingBlockPowers) {
        if (p > absoluteGatePower) {
            sum += p;
            ++count;
        }
    }
    if (count == 0) {
        m_cachedIntegrated = kFloorLufs;
        return m_cachedIntegrated;
    }

    const double relativeGatePower = (sum / static_cast<double>(count)) * 0.1;
    double gatedSum = 0.0;
    size_t gatedCount = 0;
    for (const double p : m_gatingBlockPowers) {
        if (p > absoluteGatePower && p > relativeGatePower) {
            gatedSum += p;
            ++gatedCount;
        }
    }
    m_cachedIntegrated = gatedCount == 0 ? kFloorLufs : PowerToLufs(gatedSum / static_cast<double>(gatedCount));
    return m_cachedIntegrated;
}

CcLoudness LoudnessMeter::GetReading() {
    std::lock_guard<std::mutex> lock(m_mutex);
    CcLoudness reading{};
    reading.momentaryLufs = PowerToLufs(Mean(m_recentPowers, kMomentarySubBlocks));
    reading.shortTermLufs = PowerToLufs(Mean(m_recentPowers, kShortTermSubBlocks));
    reading.integratedLufs = IntegratedLufsLocked();

    double peak = 0.0;
    for (const double p : m_recentPeaks) {
        peak = std::max(peak, p);
    }
    reading.peakDbfs = peak <= 0.0 ? kFloorLufs : std::max(kFloorLufs, 20.0 * std::log10(peak));
    return reading;
}

} // namespace capturecore
