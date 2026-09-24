/* rt_math.c — PL/I runtime library (libpli): scalar math (MOD/ROUND + musl port). */
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

/* ROUND(x, n): round x to n fractional decimal digits (n may be negative). */
double pli_round(double x, long long n) {
  double factor = 1.0;
  for (long long i = 0; i < n; ++i) factor *= 10.0;
  for (long long i = 0; i > n; --i) factor /= 10.0;
  return __builtin_round(x * factor) / factor;
}

/* Scalar math built-ins (QR2.7, <math.h> analogues): forward to the musl-port
 * implementations in rt_mathport.c so the compiler does not need to link -lm
 * for the standard transcendentals.  LOG is the natural logarithm. */
double pli_floor(double x) { return __builtin_floor(x); }
double pli_ceil(double x)  { return __builtin_ceil(x); }
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
