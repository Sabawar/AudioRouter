#pragma once
// ============================================================================
//  RNNoiseWrapper  –  Wraps the Xiph/Mozilla RNNoise neural noise suppressor
//
//  RNNoise requirements:
//   • Processes exactly 480 samples per frame at 48 kHz mono
//   • Input/output values are in INT16 scale: [-32768 .. +32767]
//   • Returns VAD probability [0..1] — usable as soft gate
//
//  This wrapper:
//   • Accepts arbitrary-length float buffers in [-1..1]
//   • Internally buffers to 480-sample chunks
//   • Applies a soft VAD gate after denoising
//   • Works per-channel (call once per channel)
// ============================================================================

#if HAVE_RNNOISE
#include "rnnoise.h"
#endif

#include <vector>
#include <cstring>
#include <cmath>
#include <algorithm>

class RNNoiseChannel {
public:
    static constexpr int kFrameSize = 480;  // RNNoise requirement

    RNNoiseChannel() { reset(); }
    ~RNNoiseChannel() { destroy(); }

    // Non-copyable
    RNNoiseChannel(const RNNoiseChannel&)            = delete;
    RNNoiseChannel& operator=(const RNNoiseChannel&) = delete;

    void reset() {
        destroy();
#if HAVE_RNNOISE
        m_state = rnnoise_create();
#endif
        m_inBuf.clear();
        m_outBuf.clear();
        m_vadSmooth = 0.0f;
    }

    void destroy() {
#if HAVE_RNNOISE
        if (m_state) { rnnoise_destroy(m_state); m_state = nullptr; }
#endif
    }

    // Process n float samples [-1,1], writes results back into buf[].
    // Returns average VAD probability for this block.
    float process(float* buf, int n, float vadGate = 0.005f) {
#if !HAVE_RNNOISE
        return 1.0f; // passthrough if not compiled in
#else
        float avgVad = 0.0f;
        int   vadCnt = 0;

        // Accumulate input
        for (int i = 0; i < n; ++i)
            m_inBuf.push_back(buf[i]);

        // Process in 480-sample chunks
        static float frame[kFrameSize];
        static float outFrame[kFrameSize];

        int written = 0;
        while ((int)m_inBuf.size() >= kFrameSize) {
            // Scale [-1,1] → [-32768,32767]
            for (int k = 0; k < kFrameSize; ++k)
                frame[k] = m_inBuf[k] * 32768.0f;

            float vad = rnnoise_process_frame(m_state, outFrame, frame);
            avgVad += vad;
            ++vadCnt;

            // Smooth VAD for soft gating (avoids hard click on voice stop)
            float vadTarget = vad > vadGate ? 1.0f : vad / vadGate;
            m_vadSmooth = 0.85f * m_vadSmooth + 0.15f * vadTarget;

            // Scale back [-32768,32767] → [-1,1]  and apply smooth VAD gate
            for (int k = 0; k < kFrameSize; ++k)
                m_outBuf.push_back((outFrame[k] / 32768.0f) * m_vadSmooth);

            // Consume processed input
            m_inBuf.erase(m_inBuf.begin(), m_inBuf.begin() + kFrameSize);
        }

        // Drain output back into buf[]
        int drain = std::min(n, (int)m_outBuf.size());
        for (int i = 0; i < drain; ++i) buf[i] = m_outBuf[i];
        if (drain < n) {
            // Output lag: zero-fill (first call only — after that pipeline fills)
            for (int i = drain; i < n; ++i) buf[i] = 0.0f;
        }
        if (drain > 0)
            m_outBuf.erase(m_outBuf.begin(), m_outBuf.begin() + drain);

        return vadCnt > 0 ? avgVad / vadCnt : m_vadSmooth;
#endif
    }

    float vadLevel() const { return m_vadSmooth; }

private:
#if HAVE_RNNOISE
    DenoiseState*      m_state     { nullptr };
#endif
    std::vector<float> m_inBuf;
    std::vector<float> m_outBuf;
    float              m_vadSmooth { 0.0f };
};
