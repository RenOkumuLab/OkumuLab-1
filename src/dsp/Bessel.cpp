/*
 * OkumuLab 1 — modified Bessel functions I_n, K_n of integer order (see Bessel.h)
 */
#include "Bessel.h"

#include <cmath>

namespace okl::bessel::detail
{

namespace
{
constexpr double kPiB = 3.14159265358979323846;
constexpr double kEulerGamma = 0.57721566490153286061;

/* K_0(x) and K_1(x), x > 0 */
double k01 (double x, double& k1)
{
    if (x <= 2.0)
    {
        /* Abramowitz & Stegun 9.6.13 and 9.6.11 (n = 1), h = x / 2:
             K_0 = -(ln h + gamma) I_0 + sum_{k>=1} h^2k / (k!)^2 H_k
             K_1 = 1/x + ln h I_1 - (x/4) sum_{k>=0} (psi(k+1) + psi(k+2)) h^2k / (k! (k+1)!)          */
        const double h = 0.5 * x, h2 = h * h, lnh = std::log (h);
        double t = 1.0, u = 1.0, H = 0.0;          // t = h^2k / (k!)^2, u = h^2k / (k! (k+1)!), H = H_k
        double i0 = 1.0, s0 = 0.0, i1 = 1.0, s1 = 2.0 * -kEulerGamma + 1.0;
        for (int k = 1; k < 40; ++k)
        {
            t *= h2 / ((double) k * k);
            u *= h2 / ((double) k * (k + 1));
            H += 1.0 / k;
            i0 += t;
            s0 += t * H;
            i1 += u;
            s1 += ((-kEulerGamma + H) + (-kEulerGamma + H + 1.0 / (k + 1))) * u;
            if (t < 1e-18 * i0 && u < 1e-18 * i1) break;
        }
        i1 *= h;
        k1 = 1.0 / x + lnh * i1 - 0.25 * x * s1;
        return -(lnh + kEulerGamma) * i0 + s0;
    }
    // Steed's continued fraction CF2 (Temme's method, order 0; Numerical Recipes' bessik)
    double b = 2.0 * (1.0 + x), d = 1.0 / b, h = d, delh = d, q1 = 0.0, q2 = 1.0;
    const double a1 = 0.25;
    double q = a1, c = a1, a = -a1, s = 1.0 + q * delh;
    for (int i = 2; i <= 10000; ++i)
    {
        a -= 2 * (i - 1);
        c = -a * c / i;
        const double qnew = (q1 - b * q2) / a;
        q1 = q2;
        q2 = qnew;
        q += c * qnew;
        b += 2.0;
        d = 1.0 / (b + a * d);
        delh = (b * d - 1.0) * delh;
        h += delh;
        const double dels = q * delh;
        s += dels;
        if (std::abs (dels / s) < 1e-16) break;
    }
    h = a1 * h;
    const double k0 = std::sqrt (kPiB / (2.0 * x)) * std::exp (-x) / s;
    k1 = k0 * (x + 0.5 - h) / x;
    return k0;
}
} // namespace

double I (int n, double x)
{
    if (n < 0) n = -n;
    if (x == 0.0) return n == 0 ? 1.0 : 0.0;
    // sum_k (x/2)^(2k+n) / (k! (k+n)!): positive terms, no cancellation
    const double h = 0.5 * std::abs (x), h2 = h * h;
    double t = std::exp (n * std::log (h) - std::lgamma (n + 1.0));
    double s = t;
    for (int k = 1; k < 100000; ++k)
    {
        t *= h2 / ((double) k * (k + n));
        s += t;
        if (t < 1e-17 * s) break;
    }
    return (x < 0 && (n & 1)) ? -s : s;
}

double K (int n, double x)
{
    if (n < 0) n = -n;
    double k1;
    double k0 = k01 (x, k1);
    if (n == 0) return k0;
    // K_{k+1} = K_{k-1} + (2k / x) K_k (stable upwards)
    for (int k = 1; k < n; ++k)
    {
        const double kn = k0 + 2.0 * k / x * k1;
        k0 = k1;
        k1 = kn;
    }
    return k1;
}

} // namespace okl::bessel::detail
