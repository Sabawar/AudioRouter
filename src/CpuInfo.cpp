#include "CpuInfo.h"
#include <intrin.h>   // __cpuid, __cpuidex (MSVC)
#include <cstring>

namespace CpuInfo {

static Features s_features;
static bool     s_init = false;

const Features& get() {
    if (s_init) return s_features;
    s_init = true;

    int info[4] = {};

    // ── Brand string (3 calls, 12 bytes each) ─────────────────────────────
    __cpuid(info, 0x80000002);
    memcpy(s_features.brand +  0, info, 16);
    __cpuid(info, 0x80000003);
    memcpy(s_features.brand + 16, info, 16);
    __cpuid(info, 0x80000004);
    memcpy(s_features.brand + 32, info, 16);

    // ── Leaf 1: SSE2, SSE4.1, AVX, FMA ───────────────────────────────────
    __cpuid(info, 1);
    int ecx1 = info[2];
    int edx1 = info[3];
    s_features.sse2  = (edx1 >> 26) & 1;
    s_features.sse41 = (ecx1 >> 19) & 1;
    s_features.fma   = (ecx1 >> 12) & 1;

    // AVX requires OS XSAVE support as well
    bool osXSave = (ecx1 >> 27) & 1;
    bool cpuAVX  = (ecx1 >> 28) & 1;
    if (osXSave && cpuAVX) {
        // Check OS has enabled YMM state (XGETBV.XCR0 bits 1-2)
        uint64_t xcr0 = _xgetbv(0);
        s_features.avx = (xcr0 & 0x06) == 0x06;
    }

    // ── Leaf 7: AVX2 ──────────────────────────────────────────────────────
    if (s_features.avx) {
        __cpuidex(info, 7, 0);
        s_features.avx2 = (info[1] >> 5) & 1;
    }

    return s_features;
}

} // namespace CpuInfo
