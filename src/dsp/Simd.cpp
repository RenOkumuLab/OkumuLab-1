/*
 * OkumuLab 1 — the baseline (SSE2 on x86-64, plain C++ elsewhere) and the choice of instruction set (see Simd.h)
 */
#include "Simd.h"
#include "Arch.h"

#include <cstdlib>
#if OKL_X86
 #include <emmintrin.h>
 #if defined(_MSC_VER)
  #include <intrin.h>
 #else
  #include <cpuid.h>
 #endif
#endif

namespace okl
{

namespace simd_detail
{
#if OKL_X86

bool cpuHasAvx2Fma()
{
#if defined(_MSC_VER)
    int r[4];
    __cpuid (r, 0);
    if (r[0] < 7) return false;
    __cpuid (r, 1);
    const bool fma = (r[2] & (1 << 12)) != 0, osxsave = (r[2] & (1 << 27)) != 0, avx = (r[2] & (1 << 28)) != 0;
    if (! (fma && osxsave && avx)) return false;
    if ((_xgetbv (0) & 6) != 6) return false;          // the OS saves XMM and YMM state
    __cpuidex (r, 7, 0);
    return (r[1] & (1 << 5)) != 0;                     // AVX2
#else
    unsigned a, b, c, d;
    if (! __get_cpuid (1, &a, &b, &c, &d)) return false;
    if (! ((c & (1u << 12)) && (c & (1u << 27)) && (c & (1u << 28)))) return false;
    unsigned lo, hi;
    __asm__ ("xgetbv" : "=a" (lo), "=d" (hi) : "c" (0));
    if ((lo & 6) != 6) return false;
    if (! __get_cpuid_count (7, 0, &a, &b, &c, &d)) return false;
    return (b & (1u << 5)) != 0;
#endif
}

void modalRingBase (int n, int na, double* zr, double* zi, const double* pr, const double* pim,
                    const double* oa, const double* ob, double* out)
{
    for (int t = 0; t < n; ++t)
    {
        __m128d acc = _mm_setzero_pd();
        for (int k = 0; k < na; k += 2)
        {
            const __m128d r = _mm_load_pd (zr + k), im = _mm_load_pd (zi + k);
            const __m128d a1 = _mm_load_pd (pr + k), b1 = _mm_load_pd (pim + k);
            const __m128d nr = _mm_sub_pd (_mm_mul_pd (a1, r), _mm_mul_pd (b1, im));
            const __m128d ni = _mm_add_pd (_mm_mul_pd (a1, im), _mm_mul_pd (b1, r));
            _mm_store_pd (zr + k, nr);
            _mm_store_pd (zi + k, ni);
            acc = _mm_add_pd (acc, _mm_add_pd (_mm_mul_pd (_mm_load_pd (oa + k), nr), _mm_mul_pd (_mm_load_pd (ob + k), ni)));
        }
        alignas(16) double s2[2];
        _mm_store_pd (s2, acc);
        out[t] = s2[0] + s2[1];
    }
}

void cmacBase (int nb, const float* xr, const float* xi, const float* lr, const float* li, const float* rr, const float* ri,
               float* aLr, float* aLi, float* aRr, float* aRi)
{
    int k = 0;
    for (; k + 4 <= nb; k += 4)
    {
        const __m128 a = _mm_loadu_ps (xr + k), b = _mm_loadu_ps (xi + k);
        const __m128 hr = _mm_loadu_ps (lr + k), hi = _mm_loadu_ps (li + k), gr = _mm_loadu_ps (rr + k), gi = _mm_loadu_ps (ri + k);
        _mm_storeu_ps (aLr + k, _mm_add_ps (_mm_loadu_ps (aLr + k), _mm_sub_ps (_mm_mul_ps (a, hr), _mm_mul_ps (b, hi))));
        _mm_storeu_ps (aLi + k, _mm_add_ps (_mm_loadu_ps (aLi + k), _mm_add_ps (_mm_mul_ps (a, hi), _mm_mul_ps (b, hr))));
        _mm_storeu_ps (aRr + k, _mm_add_ps (_mm_loadu_ps (aRr + k), _mm_sub_ps (_mm_mul_ps (a, gr), _mm_mul_ps (b, gi))));
        _mm_storeu_ps (aRi + k, _mm_add_ps (_mm_loadu_ps (aRi + k), _mm_add_ps (_mm_mul_ps (a, gi), _mm_mul_ps (b, gr))));
    }
    for (; k < nb; ++k)
    {
        const float a = xr[k], b = xi[k];
        aLr[k] += a * lr[k] - b * li[k]; aLi[k] += a * li[k] + b * lr[k];
        aRr[k] += a * rr[k] - b * ri[k]; aRi[k] += a * ri[k] + b * rr[k];
    }
}

#else   // ARM64 and other CPUs: the same arithmetic in plain C++ (the compiler vectorises the inner loops)

bool cpuHasAvx2Fma() { return false; }

void modalRingBase (int n, int na, double* zr, double* zi, const double* pr, const double* pim,
                    const double* oa, const double* ob, double* out)
{
    for (int t = 0; t < n; ++t)
    {
        double acc0 = 0, acc1 = 0;          // even and odd modes, summed as the SSE2 version's two lanes
        for (int k = 0; k < na; k += 2)
        {
            const double nr0 = pr[k] * zr[k] - pim[k] * zi[k], ni0 = pr[k] * zi[k] + pim[k] * zr[k];
            const double nr1 = pr[k + 1] * zr[k + 1] - pim[k + 1] * zi[k + 1], ni1 = pr[k + 1] * zi[k + 1] + pim[k + 1] * zr[k + 1];
            zr[k] = nr0; zi[k] = ni0; zr[k + 1] = nr1; zi[k + 1] = ni1;
            acc0 += oa[k] * nr0 + ob[k] * ni0;
            acc1 += oa[k + 1] * nr1 + ob[k + 1] * ni1;
        }
        out[t] = acc0 + acc1;
    }
}

void cmacBase (int nb, const float* xr, const float* xi, const float* lr, const float* li, const float* rr, const float* ri,
               float* aLr, float* aLi, float* aRr, float* aRi)
{
    for (int k = 0; k < nb; ++k)
    {
        const float a = xr[k], b = xi[k];
        aLr[k] += a * lr[k] - b * li[k]; aLi[k] += a * li[k] + b * lr[k];
        aRr[k] += a * rr[k] - b * ri[k]; aRi[k] += a * ri[k] + b * rr[k];
    }
}

#endif
} // namespace simd_detail

namespace
{
using namespace simd_detail;
struct Dispatch
{
    bool avx2 = cpuHasAvx2Fma() && ! std::getenv ("OKL_NO_AVX2");
    decltype (&modalRingBase) ring = avx2 ? &simd_detail::modalRingAvx2 : &modalRingBase;
    decltype (&cmacBase) mac = avx2 ? &simd_detail::cmacAvx2 : &cmacBase;
};
const Dispatch& dispatch()
{
    static const Dispatch d;     // (thread-safe initialisation; first used in prepare, not on the audio thread)
    return d;
}
} // namespace

void modalRing (int n, int na, double* zr, double* zi, const double* pr, const double* pim,
                const double* oa, const double* ob, double* out)
{
    dispatch().ring (n, na, zr, zi, pr, pim, oa, ob, out);
}

void cmac (int nb, const float* xr, const float* xi, const float* lr, const float* li, const float* rr, const float* ri,
           float* aLr, float* aLi, float* aRr, float* aRi)
{
    dispatch().mac (nb, xr, xi, lr, li, rr, ri, aLr, aLi, aRr, aRi);
}

const char* simdLevel()
{
#if OKL_X86
    return dispatch().avx2 ? "AVX2+FMA" : "SSE2";
#elif OKL_ARM64
    return "C++ (NEON)";
#else
    return "C++";
#endif
}

} // namespace okl
