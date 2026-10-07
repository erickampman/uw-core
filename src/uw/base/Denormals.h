#pragma once

#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64) || defined(__SSE__)
#include <xmmintrin.h>
#endif

namespace uw {

// Denormal floats (the tiny values a reverb's tail or a filter's decay ends
// in) flushed to zero on this thread while it lives, then the thread's mode
// restored. A CPU takes many times longer over a denormal than over a normal
// float, so an audio block full of them can overrun (StageHand's docs/macos.md, CoreAudio
// backend). Engine::process() holds one, so every backend and every plugin
// the engine calls gets it. x86: the MXCSR's flush-to-zero and
// denormals-are-zero bits; ARM64 (Apple Silicon): the FPCR's FZ bit, which
// does both. Elsewhere: nothing. Real-time safe.
class ScopedFlushDenormals {
public:
    ScopedFlushDenormals() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__SSE__)
        saved = _mm_getcsr();
        _mm_setcsr(static_cast<unsigned>(saved) | 0x8040u);   // FTZ (bit 15) | DAZ (bit 6)
#elif defined(__aarch64__)
        std::uint64_t fpcr = 0;
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
        saved = fpcr;
        __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr | (std::uint64_t{1} << 24)));   // FZ
#endif
    }
    ~ScopedFlushDenormals() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__SSE__)
        _mm_setcsr(static_cast<unsigned>(saved));
#elif defined(__aarch64__)
        __asm__ __volatile__("msr fpcr, %0" : : "r"(saved));
#endif
    }

    ScopedFlushDenormals(const ScopedFlushDenormals&)            = delete;
    ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;

private:
    std::uint64_t saved = 0;
};

} // namespace uw
