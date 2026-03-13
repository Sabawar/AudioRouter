#pragma once
// ============================================================================
//  NoiseFilter  –  real-time microphone noise suppression chain
//
//  Modes:
//   Off        – bypass
//   Light      – high-pass 85Hz + noise gate
//   Medium     – spectral subtraction + high-pass + gate
//   Aggressive – deep spectral subtraction + gate
//   RNNoise    – Xiph/Mozilla neural network (best quality, requires build with rnnoise)
// ============================================================================
#include "RNNoiseWrapper.h"
#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <atomic>

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

enum class NoiseFilterMode : int {
    Off        = 0,
    Light      = 1,
    Medium     = 2,
    Aggressive = 3,
    RNNoise    = 4,   // Neural network — best quality
};

// ──────────────────────────────────────────────────────────────────────────────
//  Biquad filter (transposed direct form II)
// ──────────────────────────────────────────────────────────────────────────────
struct Biquad {
    double b0{1}, b1{0}, b2{0}, a1{0}, a2{0};
    double z1{0}, z2{0};

    // High-pass Butterworth 2nd order
    static Biquad highPass(double sampleRate, double cutoffHz) {
        Biquad bq;
        double f  = cutoffHz / sampleRate;
        double w0 = 2.0 * M_PI * f;
        double q  = 0.7071;  // Butterworth
        double alpha = std::sin(w0) / (2.0 * q);
        double cos_w = std::cos(w0);
        double a0 = 1.0 + alpha;
        bq.b0 =  (1.0 + cos_w) / (2.0 * a0);
        bq.b1 = -(1.0 + cos_w) /       a0;
        bq.b2 =  (1.0 + cos_w) / (2.0 * a0);
        bq.a1 = (-2.0 * cos_w)       / a0;
        bq.a2 = ( 1.0 - alpha)        / a0;
        return bq;
    }

    // Low-shelf: gentle low-frequency attenuation
    static Biquad lowShelf(double sampleRate, double cutoffHz, double gainDb) {
        Biquad bq;
        double A  = std::pow(10.0, gainDb / 40.0);
        double w0 = 2.0 * M_PI * cutoffHz / sampleRate;
        double alpha = std::sin(w0) / 2.0 * std::sqrt((A + 1.0/A) * (1.0/0.9 - 1.0) + 2.0);
        double cos_w = std::cos(w0);
        double a0 = (A+1) + (A-1)*cos_w + 2*std::sqrt(A)*alpha;
        bq.b0 = (A*((A+1) - (A-1)*cos_w + 2*std::sqrt(A)*alpha)) / a0;
        bq.b1 = (2*A*((A-1) - (A+1)*cos_w)) / a0;
        bq.b2 = (A*((A+1) - (A-1)*cos_w - 2*std::sqrt(A)*alpha)) / a0;
        bq.a1 = (-2*((A-1) + (A+1)*cos_w)) / a0;
        bq.a2 = ((A+1) + (A-1)*cos_w - 2*std::sqrt(A)*alpha) / a0;
        return bq;
    }

    inline float process(float x) {
        double y = b0*x + z1;
        z1 = b1*x - a1*y + z2;
        z2 = b2*x - a2*y;
        return (float)y;
    }

    void reset() { z1 = z2 = 0.0; }
};

// ──────────────────────────────────────────────────────────────────────────────
//  Radix-2 Cooley–Tukey FFT (in-place, power-of-2 size)
// ──────────────────────────────────────────────────────────────────────────────
namespace FFTUtil {
    using Cx = std::complex<float>;

    inline void fft(std::vector<Cx>& a, bool inverse) {
        int n = (int)a.size();
        // Bit-reversal permutation
        for (int i = 1, j = 0; i < n; ++i) {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (int len = 2; len <= n; len <<= 1) {
            float ang = (float)(2.0 * M_PI / len * (inverse ? -1 : 1));
            Cx wlen(std::cos(ang), std::sin(ang));
            for (int i = 0; i < n; i += len) {
                Cx w(1.0f, 0.0f);
                for (int j = 0; j < len/2; ++j) {
                    Cx u = a[i+j], v = a[i+j+len/2] * w;
                    a[i+j]        = u + v;
                    a[i+j+len/2]  = u - v;
                    w *= wlen;
                }
            }
        }
        if (inverse) for (auto& x : a) x /= (float)n;
    }
} // namespace FFTUtil

// ──────────────────────────────────────────────────────────────────────────────
//  Spectral noise suppressor (one channel, overlap-add)
//
//  Algorithm:
//   1. Accumulate samples into an overlap buffer (hopSize = N/2)
//   2. Apply Hann window, forward FFT
//   3. Estimate noise floor with slow-attack/fast-release power tracking
//   4. Wiener-inspired gain: G[k] = max(1 - alpha * N[k]/P[k], floor)
//   5. IFFT → overlap-add → output FIFO
// ──────────────────────────────────────────────────────────────────────────────
class SpectralSuppressor {
public:
    static constexpr int kFFTSize  = 512;  // ~10.7ms @ 48kHz, power of 2
    static constexpr int kHopSize  = kFFTSize / 2;
    static constexpr int kBins     = kFFTSize / 2 + 1;

    struct Params {
        float suppressionAlpha  = 2.0f;   // how aggressively to suppress (1=medium, 3=aggressive)
        float noiseFloor        = 0.02f;  // minimum gain floor (avoid total silence)
        float noiseEstAttack    = 0.001f; // noise floor estimator: fast-attack coefficient
        float noiseEstRelease   = 0.9999f;// noise floor estimator: slow-release (tracks stationary noise)
        float smoothAlpha       = 0.6f;   // inter-frame gain smoothing (spectral de-click)
        bool  enablePresenceBoost = true; // gentle 2-4kHz boost for voice clarity after suppression
    };

    SpectralSuppressor() {
        m_fftBuf.resize(kFFTSize);
        m_window.resize(kFFTSize);
        m_inBuf.resize(kFFTSize, 0.0f);
        m_overlapBuf.resize(kFFTSize, 0.0f);
        m_outBuf.resize(kFFTSize * 4, 0.0f);
        m_noiseEst.resize(kBins, 1e-6f);
        m_prevGain.resize(kBins, 1.0f);

        // Hann window
        for (int i = 0; i < kFFTSize; ++i)
            m_window[i] = 0.5f * (1.0f - std::cos(2.0f*(float)M_PI*i / (kFFTSize-1)));

        // Calibration block: assume silence at start → init noise estimate
        // Will auto-calibrate from first few hundred ms of audio
        m_calibrationFrames = (48000 / kHopSize) / 2; // ~0.25s
    }

    void setParams(const Params& p) { m_p = p; }

    // Process one frame of `n` samples (mono).
    // Returns number of output samples ready in outBuffer.
    int process(const float* in, int n, float* out) {
        int produced = 0;
        for (int i = 0; i < n; ++i) {
            m_inBuf[m_inFill++] = in[i];
            if (m_inFill >= kHopSize) {
                // Shift old second-half → first-half, fill second-half
                processHop();
                m_inFill = 0;
            }
        }

        // Drain output FIFO
        while (m_outRead < m_outWrite && produced < n) {
            out[produced++] = m_outBuf[m_outRead++ % (kFFTSize*4)];
        }
        // Zero-pad if not enough output yet
        while (produced < n) out[produced++] = 0.0f;
        return produced;
    }

    void reset() {
        std::fill(m_inBuf.begin(),      m_inBuf.end(),      0.0f);
        std::fill(m_overlapBuf.begin(), m_overlapBuf.end(), 0.0f);
        std::fill(m_outBuf.begin(),     m_outBuf.end(),     0.0f);
        std::fill(m_noiseEst.begin(),   m_noiseEst.end(),   1e-6f);
        std::fill(m_prevGain.begin(),   m_prevGain.end(),   1.0f);
        m_inFill = 0;
        m_outWrite = 0;
        m_outRead  = 0;
        m_calibrationFrames = (48000 / kHopSize) / 2;
        m_analysisPtr = 0;
    }

private:
    void processHop() {
        // Copy analysis frame: [prev hop | new hop]
        // We maintain a double-buffer: m_analysisPtr alternates
        static std::vector<float> frame(kFFTSize, 0.0f);
        // Shift overlap
        std::copy(frame.begin() + kHopSize, frame.end(), frame.begin());
        std::copy(m_inBuf.begin(), m_inBuf.begin() + kHopSize,
                  frame.begin() + kHopSize);

        // Apply Hann window + copy to FFT buffer
        for (int k = 0; k < kFFTSize; ++k)
            m_fftBuf[k] = { frame[k] * m_window[k], 0.0f };

        FFTUtil::fft(m_fftBuf, false);

        // Compute power spectrum (positive bins only)
        std::vector<float> power(kBins);
        for (int k = 0; k < kBins; ++k)
            power[k] = m_fftBuf[k].real()*m_fftBuf[k].real()
                     + m_fftBuf[k].imag()*m_fftBuf[k].imag();

        // Calibration: during initial silence assume all is noise
        if (m_calibrationFrames > 0) {
            for (int k = 0; k < kBins; ++k)
                m_noiseEst[k] = std::max(m_noiseEst[k], power[k]);
            --m_calibrationFrames;
        }

        // Update noise floor estimate: min-stat tracking
        // Slow release follows down → tracks stationary noise (room hum, fan)
        // Fast attack tracks upward only when signal clearly exceeds noise
        for (int k = 0; k < kBins; ++k) {
            float p = power[k];
            float n = m_noiseEst[k];
            if (p < n)
                m_noiseEst[k] = m_p.noiseEstAttack * p + (1.0f - m_p.noiseEstAttack) * n;
            else
                m_noiseEst[k] = m_p.noiseEstRelease * n + (1.0f - m_p.noiseEstRelease) * p;
        }

        // Compute Wiener-inspired spectral gain
        // G[k] = max(1 - alpha * NoiseEst[k] / Power[k], floor)
        std::vector<float> gain(kBins);
        for (int k = 0; k < kBins; ++k) {
            float p = power[k];
            float nk = m_noiseEst[k];
            float g = (p > 1e-12f) ? std::max(1.0f - m_p.suppressionAlpha * nk / p,
                                                m_p.noiseFloor)
                                    : m_p.noiseFloor;
            // Temporal smoothing (prevents spectral flutter / "musical noise")
            g = m_p.smoothAlpha * m_prevGain[k] + (1.0f - m_p.smoothAlpha) * g;
            gain[k] = g;
            m_prevGain[k] = g;
        }

        // Optional: gentle presence boost 1.5-4 kHz (voice formant region)
        // to restore clarity after suppression
        if (m_p.enablePresenceBoost) {
            // Bins 16-43 ≈ 1500-4000 Hz @ 48kHz with 512-pt FFT
            for (int k = 16; k <= 43; ++k) {
                float boost = 1.0f + 0.15f * gain[k]; // gentle
                gain[k] = std::min(gain[k] * boost, 1.0f);
            }
        }

        // Apply gain to spectrum (both positive and mirrored negative bins)
        for (int k = 0; k < kBins; ++k) {
            m_fftBuf[k] *= gain[k];
            if (k > 0 && k < kFFTSize/2)
                m_fftBuf[kFFTSize - k] *= gain[k];
        }

        // IFFT
        FFTUtil::fft(m_fftBuf, true);

        // Overlap-add into output
        for (int k = 0; k < kFFTSize; ++k) {
            float s = m_fftBuf[k].real() * m_window[k];
            if (k < kHopSize) {
                // Add to overlap from previous hop
                float out = m_overlapBuf[k] + s;
                m_outBuf[m_outWrite++ % (kFFTSize*4)] = out;
            } else {
                m_overlapBuf[k - kHopSize] = s;
            }
        }
    }

    Params                            m_p;
    std::vector<FFTUtil::Cx>          m_fftBuf;
    std::vector<float>                m_window;
    std::vector<float>                m_inBuf;
    std::vector<float>                m_overlapBuf;
    std::vector<float>                m_outBuf;
    std::vector<float>                m_noiseEst;
    std::vector<float>                m_prevGain;
    int                               m_inFill   { 0 };
    size_t                            m_outWrite  { 0 };
    size_t                            m_outRead   { 0 };
    int                               m_calibrationFrames { 0 };
    int                               m_analysisPtr { 0 };
};

// ──────────────────────────────────────────────────────────────────────────────
//  Noise Gate  (RMS-based, with attack/release envelope)
// ──────────────────────────────────────────────────────────────────────────────
class NoiseGate {
public:
    struct Params {
        float thresholdRms  = 0.008f; // RMS threshold (linear)
        float attackMs      = 3.0f;
        float releaseMs     = 80.0f;
        float holdMs        = 20.0f;
        float floorGain     = 0.0f;   // 0 = mute, 0.05 = -26dB residual
    };

    void init(float sampleRate, const Params& p) {
        m_sr = sampleRate;
        m_p  = p;
        m_attackCoef  = std::exp(-1.0f / (sampleRate * p.attackMs  * 0.001f));
        m_releaseCoef = std::exp(-1.0f / (sampleRate * p.releaseMs * 0.001f));
        m_holdSamples = (int)(sampleRate * p.holdMs * 0.001f);
    }

    inline float process(float x) {
        // RMS with fast-average
        m_rms = 0.9995f * m_rms + 0.0005f * x * x;
        float rms = std::sqrt(m_rms);

        bool open = (rms > m_p.thresholdRms);
        if (open) m_holdCount = m_holdSamples;
        else if (m_holdCount > 0) { --m_holdCount; open = true; }

        float target = open ? 1.0f : m_p.floorGain;
        float coef   = (target > m_env) ? (1.0f - m_attackCoef)
                                        : (1.0f - m_releaseCoef);
        m_env += coef * (target - m_env);
        return x * m_env;
    }

    void reset() { m_rms = 0; m_env = 0; m_holdCount = 0; }

    float currentGain() const { return m_env; }

private:
    Params m_p;
    float  m_sr          { 48000.0f };
    float  m_attackCoef  { 0.99f };
    float  m_releaseCoef { 0.9998f };
    float  m_rms         { 0.0f };
    float  m_env         { 0.0f };
    int    m_holdSamples { 960 };
    int    m_holdCount   { 0 };
};

// ──────────────────────────────────────────────────────────────────────────────
//  Combined NoiseFilter — stereo, integrates everything above
// ──────────────────────────────────────────────────────────────────────────────
class NoiseFilter {
public:
    struct Settings {
        NoiseFilterMode mode       { NoiseFilterMode::Off };
        float           gateThreshDb{ -38.0f };  // noise gate threshold
        bool            showMeters  { true };
    };

    // Public state for UI display
    std::atomic<float> gateGainL  { 1.0f };
    std::atomic<float> gateGainR  { 1.0f };
    std::atomic<float> suppressL  { 1.0f }; // average spectral gain (0=fully suppressed)
    std::atomic<float> suppressR  { 1.0f };

    void init(float sampleRate = 48000.0f) {
        m_sr = sampleRate;
        rebuild();
    }

    void setMode(NoiseFilterMode m) {
        if (m_settings.mode != m) {
            m_settings.mode = m;
            rebuild();
        }
    }

    NoiseFilterMode getMode()    const { return m_settings.mode; }
    Settings&       settings()         { return m_settings; }

    // Process interleaved stereo float buffer in-place
    void processStereo(float* buf, int frames) {
        if (m_settings.mode == NoiseFilterMode::Off) return;

        // De-interleave
        m_monoL.resize(frames);
        m_monoR.resize(frames);
        for (int i = 0; i < frames; ++i) {
            m_monoL[i] = buf[i*2];
            m_monoR[i] = buf[i*2+1];
        }

        processChannel(m_monoL.data(), frames, true);
        processChannel(m_monoR.data(), frames, false);

        // Re-interleave
        for (int i = 0; i < frames; ++i) {
            buf[i*2]   = m_monoL[i];
            buf[i*2+1] = m_monoR[i];
        }
    }

private:
    void processChannel(float* buf, int frames, bool isLeft) {
        auto& hp   = isLeft ? m_hpL : m_hpR;
        auto& gate = isLeft ? m_gateL : m_gateR;
        auto& spec = isLeft ? m_specL : m_specR;
        auto& rnn  = isLeft ? m_rnnL : m_rnnR;

        // ── RNNoise mode: neural network denoising, skip other DSP ──────────
        if (m_settings.mode == NoiseFilterMode::RNNoise) {
            float vad = rnn.process(buf, frames);
            // VAD drives gate gain display
            if (isLeft) gateGainL.store(vad);
            else        gateGainR.store(vad);
            return;
        }

        // 1. High-pass filter
        for (int i = 0; i < frames; ++i)
            buf[i] = hp.process(buf[i]);

        // 2. Spectral suppression (Medium / Aggressive)
        if (m_settings.mode == NoiseFilterMode::Medium ||
            m_settings.mode == NoiseFilterMode::Aggressive) {
            static thread_local std::vector<float> tmpOut;
            tmpOut.resize(frames);
            spec.process(buf, frames, tmpOut.data());
            std::copy(tmpOut.begin(), tmpOut.end(), buf);
        }

        // 3. Noise gate
        float avgGain = 0.0f;
        for (int i = 0; i < frames; ++i) {
            buf[i] = gate.process(buf[i]);
            avgGain += gate.currentGain();
        }
        avgGain /= (float)frames;
        if (isLeft) gateGainL.store(avgGain);
        else        gateGainR.store(avgGain);
    }

    void rebuild() {
        m_hpL = Biquad::highPass(m_sr, 85.0);
        m_hpR = Biquad::highPass(m_sr, 85.0);
        m_hpL.reset(); m_hpR.reset();

        NoiseGate::Params gp;
        float threshLin = std::pow(10.0f, m_settings.gateThreshDb / 20.0f);
        gp.thresholdRms = threshLin * 0.707f;

        switch (m_settings.mode) {
        case NoiseFilterMode::Light:
            gp.attackMs  = 2.0f;  gp.releaseMs = 60.0f;
            gp.holdMs    = 30.0f; gp.floorGain = 0.0f;
            break;
        case NoiseFilterMode::Medium:
            gp.attackMs  = 3.0f;  gp.releaseMs = 100.0f;
            gp.holdMs    = 50.0f; gp.floorGain = 0.0f;
            break;
        case NoiseFilterMode::Aggressive:
            gp.attackMs  = 5.0f;  gp.releaseMs = 200.0f;
            gp.holdMs    = 80.0f; gp.floorGain = 0.0f;
            break;
        default: break;
        }
        m_gateL.init(m_sr, gp); m_gateL.reset();
        m_gateR.init(m_sr, gp); m_gateR.reset();

        SpectralSuppressor::Params sp;
        if (m_settings.mode == NoiseFilterMode::Medium) {
            sp.suppressionAlpha = 1.8f; sp.noiseFloor = 0.04f;
            sp.smoothAlpha = 0.7f;      sp.noiseEstRelease = 0.9997f;
            sp.enablePresenceBoost = true;
        } else if (m_settings.mode == NoiseFilterMode::Aggressive) {
            sp.suppressionAlpha = 3.5f; sp.noiseFloor = 0.01f;
            sp.smoothAlpha = 0.8f;      sp.noiseEstRelease = 0.9999f;
            sp.enablePresenceBoost = true;
        }
        m_specL.setParams(sp); m_specL.reset();
        m_specR.setParams(sp); m_specR.reset();

        // RNNoise: reset state when mode changes
        if (m_settings.mode == NoiseFilterMode::RNNoise) {
            m_rnnL.reset();
            m_rnnR.reset();
        }
    }

    float              m_sr { 48000.0f };
    Settings           m_settings;

    Biquad             m_hpL, m_hpR;
    NoiseGate          m_gateL, m_gateR;
    SpectralSuppressor m_specL, m_specR;
    RNNoiseChannel     m_rnnL, m_rnnR;   // neural network per channel

    std::vector<float> m_monoL, m_monoR;
};
