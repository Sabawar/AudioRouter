// AudioMath_Scalar.cpp  –  compiled WITHOUT /arch:AVX2
// Used on CPUs that don't support AVX2 (pre-2013 Intel / pre-2017 AMD)
#include "AudioMath_Impl.h"
#include <cmath>
#include <algorithm>
#include <emmintrin.h>   // SSE2 always available on x64

namespace AudioMath { namespace Scalar {

void mixAdd(float* dst, const float* src, size_t n, float gain) {
    // SSE2: process 4 floats at a time
    __m128 g = _mm_set1_ps(gain);
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m128 d = _mm_loadu_ps(dst + i);
        __m128 s = _mm_loadu_ps(src + i);
        d = _mm_add_ps(d, _mm_mul_ps(s, g));
        _mm_storeu_ps(dst + i, d);
    }
    for (; i < n; ++i)
        dst[i] += src[i] * gain;
}

void softClip(float* dst, size_t n, float gain) {
    // Scalar tanh — SSE2 has no tanh, use scalar with gain application
    for (size_t i = 0; i < n; ++i) {
        float s = dst[i] * gain;
        // Fast tanh approximation: x*(27+x*x)/(27+9*x*x)  (Padé)
        float x2 = s * s;
        s = s * (27.0f + x2) / (27.0f + 9.0f * x2);
        // clamp to [-1,1]
        if      (s >  1.0f) s =  1.0f;
        else if (s < -1.0f) s = -1.0f;
        dst[i] = s;
    }
}

void peakStereo(const float* buf, size_t frames, float& peakL, float& peakR) {
    __m128 maxL = _mm_setzero_ps();
    __m128 maxR = _mm_setzero_ps();
    size_t i = 0;
    // Process 4 stereo frames (8 floats) at a time
    for (; i + 4 <= frames; i += 4) {
        // Load 8 floats: L0 R0 L1 R1 L2 R2 L3 R3
        __m128 a = _mm_loadu_ps(buf + i*2);
        __m128 b = _mm_loadu_ps(buf + i*2 + 4);
        // abs via mask
        __m128 mask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
        a = _mm_and_ps(a, mask);
        b = _mm_and_ps(b, mask);
        // deinterleave: L0 L1 R0 R1 and L2 L3 R2 R3
        __m128 lo = _mm_shuffle_ps(a, b, _MM_SHUFFLE(2,0,2,0)); // L channels
        __m128 hi = _mm_shuffle_ps(a, b, _MM_SHUFFLE(3,1,3,1)); // R channels
        maxL = _mm_max_ps(maxL, lo);
        maxR = _mm_max_ps(maxR, hi);
    }
    // Horizontal max of 4-wide vector
    auto hmax4 = [](const __m128& v) -> float {
        __m128 t = _mm_max_ps(v, _mm_movehl_ps(v, v));
        t = _mm_max_ps(t, _mm_shuffle_ps(t, t, 1));
        return _mm_cvtss_f32(t);
    };
    peakL = hmax4(maxL);
    peakR = hmax4(maxR);
    for (; i < frames; ++i) {
        float l = buf[i*2],   r = buf[i*2+1];
        if (l < 0) l = -l;   if (r < 0) r = -r;
        if (l > peakL) peakL = l;
        if (r > peakR) peakR = r;
    }
}

void int16ToFloat(const short* src, float* dst, size_t n) {
    constexpr float kScale = 1.0f / 32768.0f;
    size_t i = 0;
    for (; i < n; ++i)
        dst[i] = src[i] * kScale;
}

void floatToInt16(const float* src, short* dst, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        float s = src[i];
        if      (s >  1.0f) s =  1.0f;
        else if (s < -1.0f) s = -1.0f;
        dst[i] = (short)(s * 32767.0f);
    }
}

}} // namespace AudioMath::Scalar
