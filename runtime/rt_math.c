/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* rt_math.c — PL/I runtime library (pli_runtime): scalar math (MOD/ROUND + musl port). */
/* Split from pli_rt.c; pli_rt.h + pli_rt_abi.def stay the single ABI source. */
#include "pli_rt.h"

/* Forward declarations for musl-port implementations in rt_mathport.c. */
extern double pli_math_sin(double);
extern double pli_math_cos(double);
extern double pli_math_tan(double);
extern double pli_math_asin(double);
extern double pli_math_acos(double);
extern double pli_math_atan(double);
extern double pli_math_atan2(double, double);
extern double pli_math_sinh(double);
extern double pli_math_cosh(double);
extern double pli_math_tanh(double);
extern double pli_math_asinh(double);
extern double pli_math_acosh(double);
extern double pli_math_atanh(double);
extern double pli_math_exp(double);
extern double pli_math_log(double);
extern double pli_math_log2(double);
extern double pli_math_log10(double);
extern double pli_math_sqrt(double);
extern double pli_math_cbrt(double);
extern double pli_math_erf(double);
extern double pli_math_erfc(double);
extern double pli_math_fmod(double, double);

long long pli_mod_ll(long long a, long long b) {
  if (b == 0) return 0;
  long long r = a % b;
  if (r != 0 && ((r < 0) != (b < 0))) r += b;  /* result takes the sign of b */
  return r;
}

/* MOD (float): a - b*floor(a/b); the result takes the sign of b. */
double pli_mod_dd(double a, double b) {
  double r = pli_math_fmod(a, b);
  if (r != 0.0 && ((r < 0.0) != (b < 0.0))) r += b;
  return r;
}

/* The rounding primitives. A compiler builtin is a single instruction on the
 * targets that have one (frintm on aarch64, roundsd on x86), so it is used
 * whenever the compiler provides it; the portable form below is the fallback
 * for a compiler without the builtin (MSVC), so the runtime still builds
 * there. The portable form costs about 1.7x a builtin call, so it is not
 * used where a builtin exists. */
#if defined(__GNUC__) || defined(__clang__)
#define PLI_HAS_ROUND_BUILTIN 1
#endif

/* Round half away from zero (the C99 round() semantics). */
#ifdef PLI_HAS_ROUND_BUILTIN
static double pli_round_to_nearest(double x) { return __builtin_round(x); }
#else
static double pli_round_to_nearest(double x) {
  if (x != x || x > 9.0e18 || x < -9.0e18)
    return x; /* NaN, or outside the range where the cast is exact */
  double f = (double)(long long)x; /* truncates toward zero */
  if (x >= 0.0)
    return (x - f >= 0.5) ? f + 1.0 : f;
  return (f - x >= 0.5) ? f - 1.0 : f;
}
#endif

/* Floor and ceiling without libm: truncate toward zero, then step. */
#ifdef PLI_HAS_ROUND_BUILTIN
double pli_floor(double x) { return __builtin_floor(x); }
double pli_ceil(double x) { return __builtin_ceil(x); }
#else
double pli_floor(double x) {
  if (x != x || x > 9.0e18 || x < -9.0e18)
    return x;
  double f = (double)(long long)x;
  return (x < 0.0 && f != x) ? f - 1.0 : f;
}
double pli_ceil(double x) {
  if (x != x || x > 9.0e18 || x < -9.0e18)
    return x;
  double f = (double)(long long)x;
  return (x > 0.0 && f != x) ? f + 1.0 : f;
}
#endif

/* ROUND(x, n): round x to n fractional decimal digits (n may be negative).
 * The digit count is bounded: a caller asking for a billion digits would
 * spin in the loop below. */
double pli_round(double x, long long n) {
  if (n > 300) n = 300;
  if (n < -300) n = -300;
  double factor = 1.0;
  for (long long i = 0; i < n; ++i) factor *= 10.0;
  for (long long i = 0; i > n; --i) factor /= 10.0;
  return pli_round_to_nearest(x * factor) / factor;
}

/* Scalar math built-ins (QR2.7, <math.h> analogues): forward to the musl-port
 * implementations in rt_mathport.c so the compiler does not need to link -lm
 * for the standard transcendentals.  LOG is the natural logarithm. */
double pli_sqrt(double x)  { return pli_math_sqrt(x); }
double pli_exp(double x)   { return pli_math_exp(x); }
double pli_log(double x)   { return pli_math_log(x); }
double pli_sin(double x)   { return pli_math_sin(x); }
double pli_cos(double x)   { return pli_math_cos(x); }
double pli_tan(double x)   { return pli_math_tan(x); }
double pli_log2(double x)  { return pli_math_log2(x); }
double pli_log10(double x) { return pli_math_log10(x); }
double pli_atan(double x)  { return pli_math_atan(x); }
double pli_asin(double x)  { return pli_math_asin(x); }
double pli_acos(double x)  { return pli_math_acos(x); }
double pli_atan2(double y, double x) { return pli_math_atan2(y, x); }
double pli_cbrt(double x)  { return pli_math_cbrt(x); }
double pli_sinh(double x)  { return pli_math_sinh(x); }
double pli_cosh(double x)  { return pli_math_cosh(x); }
double pli_tanh(double x)  { return pli_math_tanh(x); }
double pli_asinh(double x) { return pli_math_asinh(x); }
double pli_atanh(double x) { return pli_math_atanh(x); }
double pli_erf(double x)   { return pli_math_erf(x); }
double pli_erfc(double x)  { return pli_math_erfc(x); }

/* Degree trig variants (QR2.7, Appendix 1): argument/result in degrees. */
#define PLI_DEG_TO_RAD (3.14159265358979323846 / 180.0)
#define PLI_RAD_TO_DEG (180.0 / 3.14159265358979323846)
double pli_sind(double x)   { return pli_math_sin(x * PLI_DEG_TO_RAD); }
double pli_cosd(double x)   { return pli_math_cos(x * PLI_DEG_TO_RAD); }
double pli_tand(double x)   { return pli_math_tan(x * PLI_DEG_TO_RAD); }
double pli_atand(double x)  { return pli_math_atan(x) * PLI_RAD_TO_DEG; }

#if defined(_WIN32)
/* MinGW supplies sin/cos/exp/log/sqrt/pow/floor/ceil/fmod/fabs/atan2 as
 * long-double wrappers, while the UCRT import library has only the __imp_
 * slot. These definitions win over libmingwex.a, so an intrinsic-lowered
 * math call stays on UCRT's double routine. */
extern double (*__imp_sin)(double);
extern double (*__imp_cos)(double);
extern double (*__imp_exp)(double);
extern double (*__imp_log)(double);
extern double (*__imp_sqrt)(double);
extern double (*__imp_pow)(double, double);
extern double (*__imp_floor)(double);
extern double (*__imp_ceil)(double);
extern double (*__imp_fmod)(double, double);
extern double (*__imp_fabs)(double);
extern double (*__imp_atan2)(double, double);

double sin(double x) { return __imp_sin(x); }
double cos(double x) { return __imp_cos(x); }
double exp(double x) { return __imp_exp(x); }
double log(double x) { return __imp_log(x); }
double sqrt(double x) { return __imp_sqrt(x); }
double pow(double x, double y) { return __imp_pow(x, y); }
double floor(double x) { return __imp_floor(x); }
double ceil(double x) { return __imp_ceil(x); }
double fmod(double x, double y) { return __imp_fmod(x, y); }
double fabs(double x) { return __imp_fabs(x); }
double atan2(double y, double x) { return __imp_atan2(y, x); }
#endif
