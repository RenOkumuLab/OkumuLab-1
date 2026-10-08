/*
 * OkumuLab 1 — which CPU the code is compiled for
 *
 *   OKL_X86    x86-64 (Windows, Intel Macs, Linux PCs): SSE2 baseline, AVX2 + FMA chosen at run time (Simd.h)
 *   OKL_ARM64  64-bit ARM (Apple silicon, ARM Linux): plain C++ that the compiler vectorises
 */
#pragma once

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
 #define OKL_X86 1
#else
 #define OKL_X86 0
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
 #define OKL_ARM64 1
#else
 #define OKL_ARM64 0
#endif
