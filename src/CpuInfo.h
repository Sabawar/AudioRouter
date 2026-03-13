#pragma once
#include <cstdint>
#include <string>

// ============================================================================
//  CpuInfo  –  runtime CPU feature detection via CPUID
// ============================================================================
namespace CpuInfo {

struct Features {
    bool sse2  { false };
    bool sse41 { false };
    bool avx   { false };
    bool avx2  { false };
    bool fma   { false };
    char brand[64] {};
};

// Call once at startup
const Features& get();

inline bool hasAVX2() { return get().avx2; }
inline bool hasFMA()  { return get().fma;  }

} // namespace CpuInfo
