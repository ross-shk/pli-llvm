/* rt_math.c — PL/I runtime library (libpli): scalar math (MOD/ROUND + libm wrappers). */
/* Split from pli_rt.c; pli_rt.h + pli_rt_abi.def stay the single ABI source. */
#include "pli_rt.h"
#include <math.h>

long long pli_mod_ll(long long a, long long b) {
  if (b == 0) return 0;
  long long r = a % b;
  if (r != 0 && ((r < 0) != (b < 0))) r += b;  /* result takes the sign of b */
  return r;
}

/* MOD (float): a - b*floor(a/b); the result takes the sign of b. */
double pli_mod_dd(double a, double b) {
  double r = fmod(a, b);
  if (r != 0.0 && ((r < 0.0) != (b < 0.0))) r += b;
  return r;
}

/* ROUND(x, n): round x to n fractional decimal digits (n may be negative). */
double pli_round(double x, long long n) {
  double factor = 1.0;
  for (long long i = 0; i < n; ++i) factor *= 10.0;
  for (long long i = 0; i > n; --i) factor /= 10.0;
  return round(x * factor) / factor;
}

/* Scalar math built-ins (QR2.7, <math.h> analogues): thin wrappers so the
 * emitted IR ABI is the pli_* one. LOG is the natural logarithm. */
double pli_floor(double x) { return floor(x); }
double pli_ceil(double x) { return ceil(x); }
double pli_sqrt(double x) { return sqrt(x); }
double pli_exp(double x) { return exp(x); }
double pli_log(double x) { return log(x); }
double pli_sin(double x) { return sin(x); }
double pli_cos(double x) { return cos(x); }
double pli_tan(double x) { return tan(x); }
double pli_log2(double x) { return log2(x); }
double pli_log10(double x) { return log10(x); }
double pli_atan(double x) { return atan(x); }
double pli_asin(double x) { return asin(x); }
double pli_acos(double x) { return acos(x); }
double pli_atan2(double y, double x) { return atan2(y, x); }
double pli_cbrt(double x) { return cbrt(x); }
double pli_sinh(double x) { return sinh(x); }
double pli_cosh(double x) { return cosh(x); }
double pli_tanh(double x) { return tanh(x); }
double pli_atanh(double x) { return atanh(x); }
double pli_erf(double x) { return erf(x); }
double pli_erfc(double x) { return erfc(x); }

/* Degree trig variants (QR2.7, Appendix 1): argument/result in degrees. */
#define PLI_DEG_TO_RAD (3.14159265358979323846 / 180.0)
#define PLI_RAD_TO_DEG (180.0 / 3.14159265358979323846)
double pli_sind(double x) { return sin(x * PLI_DEG_TO_RAD); }
double pli_cosd(double x) { return cos(x * PLI_DEG_TO_RAD); }
double pli_tand(double x) { return tan(x * PLI_DEG_TO_RAD); }
double pli_atand(double x) { return atan(x) * PLI_RAD_TO_DEG; }

/* REPEAT(s, n): fill dst with n copies of s, blank-padding to dstcap. */

