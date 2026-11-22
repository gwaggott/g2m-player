// Flush denormal floats to zero while in scope (x86 FTZ + DAZ, ARM FZ):
// put one at the top of each audio callback. Denormals can make the
// network's arithmetic many times slower as state decays in silence;
// flushing them changes outputs by far less than float rounding.
#pragma once

#include <cstdint>

#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#include <xmmintrin.h>
#define G2M_DENORMALS_X86 1
#endif

namespace g2m {

class FlushDenormals {
public:
    FlushDenormals() {
#if defined(G2M_DENORMALS_X86)
        saved_ = _mm_getcsr();
        _mm_setcsr(saved_ | 0x8040);  // FTZ (bit 15) and DAZ (bit 6)
#elif defined(__aarch64__)
        std::uint64_t fpcr;
        asm volatile("mrs %0, fpcr" : "=r"(fpcr));
        saved_ = fpcr;
        asm volatile("msr fpcr, %0" : : "r"(fpcr | (1ull << 24)));  // FZ
#elif defined(__arm__) && defined(__ARM_FP)
        std::uint32_t fpscr;
        asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
        saved_ = fpscr;
        asm volatile("vmsr fpscr, %0" : : "r"(fpscr | (1u << 24)));  // FZ
#endif
    }

    ~FlushDenormals() {
#if defined(G2M_DENORMALS_X86)
        _mm_setcsr(static_cast<unsigned>(saved_));
#elif defined(__aarch64__)
        asm volatile("msr fpcr, %0" : : "r"(saved_));
#elif defined(__arm__) && defined(__ARM_FP)
        asm volatile("vmsr fpscr, %0" : : "r"(static_cast<std::uint32_t>(saved_)));
#endif
    }

    FlushDenormals(const FlushDenormals&) = delete;
    FlushDenormals& operator=(const FlushDenormals&) = delete;

private:
    std::uint64_t saved_ = 0;
};

}  // namespace g2m
