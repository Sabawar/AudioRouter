#include "AudioMath.h"
#include "AudioMath_Impl.h"
#include "CpuInfo.h"
#include <cstddef>

namespace AudioMath {

// ── Function pointers – set by init() ────────────────────────────────────────
static void (*fn_mixAdd)      (float*, const float*, size_t, float)        = nullptr;
static void (*fn_softClip)    (float*, size_t, float)                      = nullptr;
static void (*fn_peakStereo)  (const float*, size_t, float&, float&)       = nullptr;
static void (*fn_int16ToFloat)(const short*, float*, size_t)               = nullptr;
static void (*fn_floatToInt16)(const float*, short*, size_t)               = nullptr;

static const char* s_activePath = "Scalar";

// ── init() ────────────────────────────────────────────────────────────────────
void init() {
    if (CpuInfo::hasAVX2() && CpuInfo::hasFMA()) {
        fn_mixAdd       = AVX2::mixAdd;
        fn_softClip     = AVX2::softClip;
        fn_peakStereo   = AVX2::peakStereo;
        fn_int16ToFloat = AVX2::int16ToFloat;
        fn_floatToInt16 = AVX2::floatToInt16;
        s_activePath    = "AVX2+FMA";
    } else {
        fn_mixAdd       = Scalar::mixAdd;
        fn_softClip     = Scalar::softClip;
        fn_peakStereo   = Scalar::peakStereo;
        fn_int16ToFloat = Scalar::int16ToFloat;
        fn_floatToInt16 = Scalar::floatToInt16;
        s_activePath    = CpuInfo::get().sse2 ? "SSE2" : "Scalar";
    }
}

const char* activePath() { return s_activePath; }

// ── Public API – forward to selected implementation ───────────────────────────
void mixAdd(float* dst, const float* src, size_t n, float gain) {
    fn_mixAdd(dst, src, n, gain);
}
void softClip(float* dst, size_t n, float gain) {
    fn_softClip(dst, n, gain);
}
void peakStereo(const float* buf, size_t frames, float& peakL, float& peakR) {
    fn_peakStereo(buf, frames, peakL, peakR);
}
void int16ToFloat(const short* src, float* dst, size_t n) {
    fn_int16ToFloat(src, dst, n);
}
void floatToInt16(const float* src, short* dst, size_t n) {
    fn_floatToInt16(src, dst, n);
}

} // namespace AudioMath
