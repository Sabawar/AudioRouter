#pragma once
#include <cstddef>

// Internal: function signatures implemented by both Scalar and AVX2 backends.
// Not part of the public API — use AudioMath.h instead.

namespace AudioMath {

namespace Scalar {
    void mixAdd      (float* dst, const float* src, size_t n, float gain);
    void softClip    (float* dst, size_t n, float gain);
    void peakStereo  (const float* buf, size_t frames, float& peakL, float& peakR);
    void int16ToFloat(const short* src, float* dst, size_t n);
    void floatToInt16(const float* src, short* dst, size_t n);
}

namespace AVX2 {
    void mixAdd      (float* dst, const float* src, size_t n, float gain);
    void softClip    (float* dst, size_t n, float gain);
    void peakStereo  (const float* buf, size_t frames, float& peakL, float& peakR);
    void int16ToFloat(const short* src, float* dst, size_t n);
    void floatToInt16(const float* src, short* dst, size_t n);
}

} // namespace AudioMath
