#pragma once
#include <cstddef>

// ============================================================================
//  AudioMath  –  runtime-dispatched SIMD audio processing
//
//  Call AudioMath::init() once at startup (after CpuInfo::get()).
//  Then use the free functions below — they automatically call
//  the AVX2 or scalar implementation based on what the CPU supports.
// ============================================================================
namespace AudioMath {

// Select implementation (scalar / AVX2) based on CPU features
void init();

// Returns "AVX2+FMA" or "SSE2" or "Scalar"
const char* activePath();

// ── Core operations (hot path in mix thread) ─────────────────────────────

// dst[i] += src[i] * gain   (mix one source into accumulation buffer)
void mixAdd(float* dst, const float* src, size_t samples, float gain);

// dst[i] = clamp(tanh(dst[i] * gain), -1, 1)   (soft-clip master bus)
void softClip(float* dst, size_t samples, float gain);

// Compute peak L/R from interleaved stereo buffer
void peakStereo(const float* buf, size_t frames,
                float& peakL, float& peakR);

// Convert int16 → float32 block
void int16ToFloat(const short* src, float* dst, size_t count);

// Convert float32 → int16 block (with clamp)
void floatToInt16(const float* src, short* dst, size_t count);

} // namespace AudioMath
