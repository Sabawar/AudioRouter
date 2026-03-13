// AudioMath_AVX2.cpp  –  compiled WITH /arch:AVX2
// Only CALLED at runtime if CPUID confirms AVX2 support.
// The OS may still load this file, but the code is never executed
// on machines that lack AVX2 — so no illegal instruction crash.
#include "AudioMath_Impl.h"
#include <immintrin.h>   // AVX2 + FMA

namespace AudioMath { namespace AVX2 {

void mixAdd(float* dst, const float* src, size_t n, float gain) {
    __m256 g = _mm256_set1_ps(gain);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256 d = _mm256_loadu_ps(dst + i);
        __m256 s = _mm256_loadu_ps(src + i);
        // FMA: d = d + s*g  (one instruction, better throughput)
        d = _mm256_fmadd_ps(s, g, d);
        _mm256_storeu_ps(dst + i, d);
    }
    // scalar tail
    for (; i < n; ++i)
        dst[i] += src[i] * gain;
}

void softClip(float* dst, size_t n, float gain) {
    __m256 g     = _mm256_set1_ps(gain);
    __m256 c27   = _mm256_set1_ps(27.0f);
    __m256 c9    = _mm256_set1_ps(9.0f);
    __m256 one   = _mm256_set1_ps(1.0f);
    __m256 mone  = _mm256_set1_ps(-1.0f);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256 s  = _mm256_mul_ps(_mm256_loadu_ps(dst + i), g);
        __m256 s2 = _mm256_mul_ps(s, s);
        // Padé tanh: s*(27+s2)/(27+9*s2)
        __m256 num = _mm256_fmadd_ps(s2, one, c27);   // 27+s2
        num = _mm256_mul_ps(s, num);                   // s*(27+s2)
        __m256 den = _mm256_fmadd_ps(s2, c9, c27);    // 27+9*s2  (FMA)
        s = _mm256_div_ps(num, den);
        // clamp
        s = _mm256_max_ps(mone, _mm256_min_ps(one, s));
        _mm256_storeu_ps(dst + i, s);
    }
    for (; i < n; ++i) {
        float s = dst[i] * gain;
        float x2 = s * s;
        s = s * (27.0f + x2) / (27.0f + 9.0f * x2);
        if      (s >  1.0f) s =  1.0f;
        else if (s < -1.0f) s = -1.0f;
        dst[i] = s;
    }
}

void peakStereo(const float* buf, size_t frames, float& peakL, float& peakR) {
    __m256 maxL = _mm256_setzero_ps();
    __m256 maxR = _mm256_setzero_ps();
    __m256 absMask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
    size_t i = 0;
    // 4 stereo frames = 8 floats per channel vector
    for (; i + 4 <= frames; i += 4) {
        // L0 R0 L1 R1 L2 R2 L3 R3 – 8 floats = one 256-bit load
        __m256 ab = _mm256_loadu_ps(buf + i * 2);
        ab = _mm256_and_ps(ab, absMask);   // abs
        // Deinterleave with permute: even positions=L, odd=R
        __m256 l = _mm256_shuffle_ps(ab, ab, _MM_SHUFFLE(2,2,0,0));
        __m256 r = _mm256_shuffle_ps(ab, ab, _MM_SHUFFLE(3,3,1,1));
        maxL = _mm256_max_ps(maxL, l);
        maxR = _mm256_max_ps(maxR, r);
    }
    // Horizontal max of 8-wide vector → scalar
    auto hmax8 = [](const __m256& v) -> float {
        __m128 lo = _mm256_castps256_ps128(v);
        __m128 hi = _mm256_extractf128_ps(v, 1);
        __m128 t  = _mm_max_ps(lo, hi);
        t = _mm_max_ps(t, _mm_movehl_ps(t, t));
        t = _mm_max_ps(t, _mm_shuffle_ps(t, t, 1));
        return _mm_cvtss_f32(t);
    };
    peakL = hmax8(maxL);
    peakR = hmax8(maxR);
    for (; i < frames; ++i) {
        float l = buf[i*2],   r = buf[i*2+1];
        if (l < 0) l = -l;   if (r < 0) r = -r;
        if (l > peakL) peakL = l;
        if (r > peakR) peakR = r;
    }
}

void int16ToFloat(const short* src, float* dst, size_t n) {
    const __m256 kScale = _mm256_set1_ps(1.0f / 32768.0f);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        // Load 8 int16 → __m128i → sign-extend to __m256i → cvt to float
        __m128i s16 = _mm_loadu_si128((const __m128i*)(src + i));
        __m256i s32 = _mm256_cvtepi16_epi32(s16);
        __m256  f   = _mm256_cvtepi32_ps(s32);
        _mm256_storeu_ps(dst + i, _mm256_mul_ps(f, kScale));
    }
    for (; i < n; ++i)
        dst[i] = src[i] / 32768.0f;
}

void floatToInt16(const float* src, short* dst, size_t n) {
    const __m256 kScale = _mm256_set1_ps(32767.0f);
    const __m256 kOne   = _mm256_set1_ps(1.0f);
    const __m256 kMOne  = _mm256_set1_ps(-1.0f);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256 f   = _mm256_loadu_ps(src + i);
        f = _mm256_max_ps(kMOne, _mm256_min_ps(kOne, f));
        f = _mm256_mul_ps(f, kScale);
        __m256i i32 = _mm256_cvtps_epi32(f);
        // Pack 8×int32 → 8×int16
        __m128i lo = _mm256_castsi256_si128(i32);
        __m128i hi = _mm256_extracti128_si256(i32, 1);
        __m128i packed = _mm_packs_epi32(lo, hi);
        _mm_storeu_si128((__m128i*)(dst + i), packed);
    }
    for (; i < n; ++i) {
        float s = src[i];
        if      (s >  1.0f) s =  1.0f;
        else if (s < -1.0f) s = -1.0f;
        dst[i] = (short)(s * 32767.0f);
    }
}

}} // namespace AudioMath::AVX2
