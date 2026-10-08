/*
 * OkumuLab 1 — AVX2 + FMA versions of the hot loops (see Simd.h). This file alone is
 * compiled for AVX2; it runs only where Simd.cpp has found AVX2, FMA and OS support.
 * On CPUs other than x86-64 the two names run the baseline (never chosen there: cpuHasAvx2Fma() is false).
 */
#include "Simd.h"
#include "Arch.h"

#if OKL_X86
 #include <immintrin.h>
#endif

namespace okl
{
namespace simd_detail
{
#if OKL_X86

void modalRingAvx2 (int n, int na, double* zr, double* zi, const double* pr, const double* pim,
                    const double* oa, const double* ob, double* out)
{
    const int n4 = na & ~3;
    for (int t = 0; t < n; ++t)
    {
        __m256d acc = _mm256_setzero_pd();
        int k = 0;
        for (; k < n4; k += 4)
        {
            const __m256d r = _mm256_loadu_pd (zr + k), im = _mm256_loadu_pd (zi + k);
            const __m256d a1 = _mm256_loadu_pd (pr + k), b1 = _mm256_loadu_pd (pim + k);
            const __m256d nr = _mm256_fmsub_pd (a1, r, _mm256_mul_pd (b1, im));      // a r - b i
            const __m256d ni = _mm256_fmadd_pd (a1, im, _mm256_mul_pd (b1, r));      // a i + b r
            _mm256_storeu_pd (zr + k, nr);
            _mm256_storeu_pd (zi + k, ni);
            acc = _mm256_fmadd_pd (_mm256_loadu_pd (oa + k), nr, acc);
            acc = _mm256_fmadd_pd (_mm256_loadu_pd (ob + k), ni, acc);
        }
        __m128d s = _mm_add_pd (_mm256_castpd256_pd128 (acc), _mm256_extractf128_pd (acc, 1));
        if (k < na)          // the last pair (na is even)
        {
            const __m128d r = _mm_loadu_pd (zr + k), im = _mm_loadu_pd (zi + k);
            const __m128d a1 = _mm_loadu_pd (pr + k), b1 = _mm_loadu_pd (pim + k);
            const __m128d nr = _mm_fmsub_pd (a1, r, _mm_mul_pd (b1, im));
            const __m128d ni = _mm_fmadd_pd (a1, im, _mm_mul_pd (b1, r));
            _mm_storeu_pd (zr + k, nr);
            _mm_storeu_pd (zi + k, ni);
            s = _mm_fmadd_pd (_mm_loadu_pd (oa + k), nr, s);
            s = _mm_fmadd_pd (_mm_loadu_pd (ob + k), ni, s);
        }
        out[t] = _mm_cvtsd_f64 (_mm_add_sd (s, _mm_unpackhi_pd (s, s)));
    }
    _mm256_zeroupper();
}

void cmacAvx2 (int nb, const float* xr, const float* xi, const float* lr, const float* li, const float* rr, const float* ri,
               float* aLr, float* aLi, float* aRr, float* aRi)
{
    int k = 0;
    for (; k + 8 <= nb; k += 8)
    {
        const __m256 a = _mm256_loadu_ps (xr + k), b = _mm256_loadu_ps (xi + k);
        const __m256 hr = _mm256_loadu_ps (lr + k), hi = _mm256_loadu_ps (li + k), gr = _mm256_loadu_ps (rr + k), gi = _mm256_loadu_ps (ri + k);
        _mm256_storeu_ps (aLr + k, _mm256_fmadd_ps (a, hr, _mm256_fnmadd_ps (b, hi, _mm256_loadu_ps (aLr + k))));
        _mm256_storeu_ps (aLi + k, _mm256_fmadd_ps (a, hi, _mm256_fmadd_ps (b, hr, _mm256_loadu_ps (aLi + k))));
        _mm256_storeu_ps (aRr + k, _mm256_fmadd_ps (a, gr, _mm256_fnmadd_ps (b, gi, _mm256_loadu_ps (aRr + k))));
        _mm256_storeu_ps (aRi + k, _mm256_fmadd_ps (a, gi, _mm256_fmadd_ps (b, gr, _mm256_loadu_ps (aRi + k))));
    }
    for (; k < nb; ++k)
    {
        const float a = xr[k], b = xi[k];
        aLr[k] += a * lr[k] - b * li[k]; aLi[k] += a * li[k] + b * lr[k];
        aRr[k] += a * rr[k] - b * ri[k]; aRi[k] += a * ri[k] + b * rr[k];
    }
    _mm256_zeroupper();
}

#else

void modalRingAvx2 (int n, int na, double* zr, double* zi, const double* pr, const double* pim,
                    const double* oa, const double* ob, double* out)
{
    modalRingBase (n, na, zr, zi, pr, pim, oa, ob, out);
}

void cmacAvx2 (int nb, const float* xr, const float* xi, const float* lr, const float* li, const float* rr, const float* ri,
               float* aLr, float* aLi, float* aRr, float* aRi)
{
    cmacBase (nb, xr, xi, lr, li, rr, ri, aLr, aLi, aRr, aRi);
}

#endif
} // namespace simd_detail
} // namespace okl
