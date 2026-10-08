/*
 * OkumuLab 1 — the two hot inner loops with a runtime choice of instruction set (P5)
 *
 *   modalRing  the struck wall ringing freely: every mode's complex state turned by its
 *              pole each sample, the output the modes' sum (Mallet.cpp)
 *   cmac       the reverb's frequency-domain multiply-accumulate, one partition of the
 *              impulse response into both ears (Reverb.cpp)
 *
 * On x86-64 the baseline is SSE2 (every x64 CPU). On CPUs with AVX2 and FMA (and an OS that saves the
 * YMM registers) the AVX2 versions in SimdAvx2.cpp are used, chosen once at start.
 * On other CPUs (Apple silicon, ARM Linux) the baseline is plain C++ that the compiler vectorises (NEON).
 */
#pragma once

namespace okl
{

/* n samples of free ringing of na modes (na even, arrays 32-byte aligned):
   z <- p z, out[t] = sum (oa Re z + ob Im z) */
void modalRing (int n, int na, double* zr, double* zi, const double* pr, const double* pim,
                const double* oa, const double* ob, double* out);

/* accL += x * hL, accR += x * hR (complex, split re/im arrays of nb bins) */
void cmac (int nb, const float* xr, const float* xi, const float* lr, const float* li, const float* rr, const float* ri,
           float* aLr, float* aLi, float* aRr, float* aRi);

/* "AVX2+FMA", "SSE2" or "C++ (NEON)": what this CPU runs (OKL_NO_AVX2=1 in the environment forces SSE2) */
const char* simdLevel();

namespace simd_detail      // (both versions, for the checks)
{
bool cpuHasAvx2Fma();      // (false on CPUs other than x86-64: there the AVX2 names run the baseline)
/* the baseline: SSE2 on x86-64, plain C++ elsewhere */
void modalRingBase (int n, int na, double* zr, double* zi, const double* pr, const double* pim,
                    const double* oa, const double* ob, double* out);
void cmacBase (int nb, const float* xr, const float* xi, const float* lr, const float* li, const float* rr, const float* ri,
               float* aLr, float* aLi, float* aRr, float* aRi);
void modalRingAvx2 (int n, int na, double* zr, double* zi, const double* pr, const double* pim,
                    const double* oa, const double* ob, double* out);
void cmacAvx2 (int nb, const float* xr, const float* xi, const float* lr, const float* li, const float* rr, const float* ri,
               float* aLr, float* aLi, float* aRr, float* aRi);
}

} // namespace okl
