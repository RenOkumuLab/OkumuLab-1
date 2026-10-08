/*
 * OkumuLab 1 — the floating-point mode the engine computes in, on every CPU
 *
 * A host's thread may round otherwise, unmask exceptions or keep denormals; the engine sets round to nearest with
 * exceptions masked (and, for audio, denormals flushed to zero) and gives the caller's mode back afterwards.
 *   x86-64: MXCSR (rounding bits 13-14, exception masks 0x1F80, FTZ 0x8000 + DAZ 0x0040)
 *   ARM64:  FPCR  (RMode bits 22-23, trap enables bits 8-12 and 15, FZ bit 24: denormal inputs and results to zero)
 * Other CPUs: the thread's mode is left as it is.
 */
#pragma once

#include "Arch.h"

#include <cstdint>

#if OKL_X86
 #include <xmmintrin.h>
#endif

namespace okl::fpenv
{

#if OKL_X86

using State = unsigned;
inline State get() { return _mm_getcsr(); }
inline void set (State s) { _mm_setcsr (s); }
/* round to nearest, exceptions masked; denormals flushed or kept */
inline State engine (State, bool flushDenormals) { return 0x1F80u | (flushDenormals ? 0x8040u : 0u); }
/* round to nearest, exceptions masked, the caller's denormal mode kept */
inline State nearest (State s) { return (s & 0x8040u) | 0x1F80u; }
/* the caller's mode with denormals flushed */
inline State flushing (State s) { return s | 0x8040u; }

#elif OKL_ARM64 && (defined(__GNUC__) || defined(__clang__))

using State = std::uint64_t;
inline State get() { State v; __asm__ __volatile__ ("mrs %0, fpcr" : "=r" (v)); return v; }
inline void set (State s) { __asm__ __volatile__ ("msr fpcr, %0" : : "r" (s)); }
constexpr State kFZ = State (1) << 24, kRMode = State (3) << 22, kTraps = State (0x9F00);
inline State engine (State s, bool flushDenormals) { return (s & ~(kFZ | kRMode | kTraps)) | (flushDenormals ? kFZ : 0); }
inline State nearest (State s) { return s & ~(kRMode | kTraps); }
inline State flushing (State s) { return s | kFZ; }

#else

using State = int;
inline State get() { return 0; }
inline void set (State) {}
inline State engine (State s, bool) { return s; }
inline State nearest (State s) { return s; }
inline State flushing (State s) { return s; }

#endif

/* round to nearest, exceptions masked; denormals flushed (audio) or kept (set-up) */
struct ScopedEngineMode
{
    const State saved = get();
    explicit ScopedEngineMode (bool flushDenormals) { set (engine (saved, flushDenormals)); }
    ~ScopedEngineMode() { set (saved); }
    ScopedEngineMode (const ScopedEngineMode&) = delete;
    ScopedEngineMode& operator= (const ScopedEngineMode&) = delete;
};

/* round to nearest, exceptions masked, the caller's denormal mode kept */
struct ScopedNearest
{
    const State saved = get();
    ScopedNearest() { set (nearest (saved)); }
    ~ScopedNearest() { set (saved); }
    ScopedNearest (const ScopedNearest&) = delete;
    ScopedNearest& operator= (const ScopedNearest&) = delete;
};

} // namespace okl::fpenv
