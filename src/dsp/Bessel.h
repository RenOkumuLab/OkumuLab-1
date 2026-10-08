/*
 * OkumuLab 1 — Bessel functions of integer order n >= 0 at x > 0 (the struck wall's modes, Mallet.cpp)
 *
 * Where the standard library has the C++17 special functions (MSVC, libstdc++: Windows, Linux) they are used.
 * Apple's libc++ has none; there J_n and Y_n come from the C library (jn, yn) and I_n, K_n from bessel::detail:
 * I_n by its power series (positive terms), K_n from K_0 and K_1 (power series below x = 2, Steed's continued
 * fraction above, as Temme's method) by upward recurrence.
 */
#pragma once

#include <cmath>
#include <version>

namespace okl::bessel
{

namespace detail      // (always compiled: the checks compare them with the standard library where it has them)
{
double I (int n, double x);
double K (int n, double x);
}

#if defined(__cpp_lib_math_special_functions)
inline double J (int n, double x) { return std::cyl_bessel_j ((double) n, x); }
inline double Y (int n, double x) { return std::cyl_neumann ((double) n, x); }
inline double I (int n, double x) { return std::cyl_bessel_i ((double) n, x); }
inline double K (int n, double x) { return std::cyl_bessel_k ((double) n, x); }
#else
inline double J (int n, double x) { return ::jn (n, x); }
inline double Y (int n, double x) { return ::yn (n, x); }
inline double I (int n, double x) { return detail::I (n, x); }
inline double K (int n, double x) { return detail::K (n, x); }
#endif

} // namespace okl::bessel
