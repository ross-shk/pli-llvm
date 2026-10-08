/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* pli_wide128.h — portable 128-bit integer (ADR-191 Windows compat).
 *
 * Wide (>18 digit) FIXED DECIMAL values are held in 128 bits. GCC/Clang
 * spell that `__int128`, which MSVC lacks (clang-cl supports it natively),
 * so the portable (lo, hi) pair is used only on true MSVC builds: identical
 * layout (16 bytes, 8-aligned, little-endian halves). All arithmetic goes
 * through the helpers below so compiler and runtime sources build unchanged
 * on every toolchain: the portable implementation is used on MSVC, native
 * operators elsewhere.
 *
 * Only the operations ADR-191 needs are provided: add/sub/mul (low 128 bits),
 * unsigned divmod (shift-subtract, host-side use only — never in hot paths),
 * magnitude/negation, decimal digit extraction, and conversions to i64,
 * double (exactly rounded via decimal + strtod), and lo/hi u64 halves (for
 * the LLVM i128 constant builder). Signed division truncates toward zero,
 * matching C++ and LLVM sdiv.
 */
#ifndef PLI_WIDE128_H
#define PLI_WIDE128_H

#include <stdint.h>

#if (defined(_MSC_VER) && !defined(__clang__)) || defined(PLI_WIDE128_FORCE_PORTABLE)
typedef struct PliU128 {
  uint64_t lo;
  uint64_t hi;
} PliU128;
typedef struct PliI128 {
  uint64_t lo;
  int64_t hi;
#ifdef __cplusplus
  PliI128() : lo(0), hi(0) {}
  PliI128(long long v) : lo((uint64_t)v), hi(v < 0 ? (int64_t)-1 : (int64_t)0) {}
#endif
} PliI128;
#define PLI_WIDE128_PORTABLE 1
#else
typedef unsigned __int128 PliU128;
typedef __int128 PliI128;
#endif

/* --- construction / predicates / bit-casts ------------------------------- */

static inline PliU128 pli_u128(uint64_t lo, uint64_t hi) {
#ifdef PLI_WIDE128_PORTABLE
  PliU128 r;
  r.lo = lo;
  r.hi = hi;
  return r;
#else
  return ((PliU128)hi << 64) | (PliU128)lo;
#endif
}

static inline PliI128 pli_from_i64(long long v) {
#ifdef PLI_WIDE128_PORTABLE
  PliI128 r;
  r.lo = (uint64_t)v;
  r.hi = v < 0 ? (int64_t)-1 : (int64_t)0;
  return r;
#else
  return (PliI128)v;
#endif
}

static inline int pli_iszero_u(PliU128 v) {
#ifdef PLI_WIDE128_PORTABLE
  return v.lo == 0 && v.hi == 0;
#else
  return v == 0;
#endif
}

static inline int pli_iszero_s(PliI128 v) {
#ifdef PLI_WIDE128_PORTABLE
  return v.lo == 0 && v.hi == 0;
#else
  return v == 0;
#endif
}

static inline int pli_isneg_s(PliI128 v) {
#ifdef PLI_WIDE128_PORTABLE
  return v.hi < 0;
#else
  return v < 0;
#endif
}

static inline PliU128 pli_to_u(PliI128 v) {
#ifdef PLI_WIDE128_PORTABLE
  PliU128 r;
  r.lo = v.lo;
  r.hi = (uint64_t)v.hi;
  return r;
#else
  return (PliU128)v;
#endif
}

static inline PliI128 pli_to_s(PliU128 v) {
#ifdef PLI_WIDE128_PORTABLE
  PliI128 r;
  r.lo = v.lo;
  r.hi = (int64_t)v.hi;
  return r;
#else
  return (PliI128)v;
#endif
}

/* --- unsigned core ------------------------------------------------------- */

static inline PliU128 pli_add_u(PliU128 a, PliU128 b) {
#ifdef PLI_WIDE128_PORTABLE
  PliU128 r;
  r.lo = a.lo + b.lo;
  r.hi = a.hi + b.hi + (r.lo < a.lo ? (uint64_t)1 : (uint64_t)0);
  return r;
#else
  return a + b;
#endif
}

static inline PliU128 pli_sub_u(PliU128 a, PliU128 b) {
#ifdef PLI_WIDE128_PORTABLE
  PliU128 r;
  r.lo = a.lo - b.lo;
  r.hi = a.hi - b.hi - (a.lo < b.lo ? (uint64_t)1 : (uint64_t)0);
  return r;
#else
  return a - b;
#endif
}

static inline PliU128 pli_neg_u(PliU128 v) {
#ifdef PLI_WIDE128_PORTABLE
  PliU128 r;
  r.lo = 0u - v.lo;
  r.hi = 0u - v.hi - (v.lo != 0 ? (uint64_t)1 : (uint64_t)0);
  return r;
#else
  return (PliU128)0 - v;
#endif
}

static inline int pli_cmp_u(PliU128 a, PliU128 b) {
#ifdef PLI_WIDE128_PORTABLE
  if (a.hi != b.hi)
    return a.hi < b.hi ? -1 : 1;
  if (a.lo != b.lo)
    return a.lo < b.lo ? -1 : 1;
  return 0;
#else
  return a < b ? -1 : a > b ? 1 : 0;
#endif
}

static inline PliU128 pli_mul_u(PliU128 a, PliU128 b) {
#ifdef PLI_WIDE128_PORTABLE
  /* 32-bit schoolbook multiply, keeping the low 128 bits. Every partial
   * product and the running sum stay below 2^34, so u64 never overflows:
   * t = r[k] + x[i]*y[j](split into 32-bit halves) + carry(<2^33). */
  uint32_t x[4], y[4], r[4];
  uint64_t t;
  uint64_t carry;
  int i, j;
  x[0] = (uint32_t)a.lo;
  x[1] = (uint32_t)(a.lo >> 32);
  x[2] = (uint32_t)a.hi;
  x[3] = (uint32_t)(a.hi >> 32);
  y[0] = (uint32_t)b.lo;
  y[1] = (uint32_t)(b.lo >> 32);
  y[2] = (uint32_t)b.hi;
  y[3] = (uint32_t)(b.hi >> 32);
  r[0] = r[1] = r[2] = r[3] = 0;
  for (i = 0; i < 4; i++) {
    carry = 0;
    for (j = 0; j + i < 4; j++) {
      uint64_t p = (uint64_t)x[i] * (uint64_t)y[j];
      t = (uint64_t)r[i + j] + (p & 0xFFFFFFFFu) + carry;
      r[i + j] = (uint32_t)t;
      carry = (t >> 32) + (p >> 32);
    }
  }
  {
    PliU128 q;
    q.lo = ((uint64_t)r[1] << 32) | r[0];
    q.hi = ((uint64_t)r[3] << 32) | r[2];
    return q;
  }
#else
  return a * b;
#endif
}

/* Unsigned divmod (d != 0): restoring shift-subtract. The remainder is kept
 * 129 bits wide (`carry` + 128) so a divisor >= 2^127 stays exact. */
static inline void pli_divmod_u(PliU128 n, PliU128 d, PliU128 *q, PliU128 *r) {
#ifdef PLI_WIDE128_PORTABLE
  uint64_t qlo = 0, qhi = 0, rlo = 0, rhi = 0, carry = 0;
  int i;
  for (i = 127; i >= 0; --i) {
    uint64_t bit = (i >= 64 ? (n.hi >> (i - 64)) : (n.lo >> i)) & (uint64_t)1;
    uint64_t blo, bhi, b0, b1;
    /* R is < d here, so the shift keeps T = carry:r in 129 bits. */
    carry = rhi >> 63;
    rhi = (rhi << 1) | (rlo >> 63);
    rlo = (rlo << 1) | bit;
    /* T >= d iff carry==1 or r >= d (d < 2^128). */
    if (carry || pli_cmp_u(pli_u128(rlo, rhi), d) >= 0) {
      /* Commit T -= d = (carry-b1):(r-d mod 2^128). carry-b1 cannot go
       * negative here: carry==1 absorbs any borrow, and carry==0 implies
       * r >= d, i.e. b1==0. */
      blo = rlo - d.lo;
      b0 = (rlo < d.lo) ? (uint64_t)1 : (uint64_t)0;
      bhi = rhi - d.hi - b0;
      b1 = (rhi < d.hi) || (b0 && rhi == d.hi) ? (uint64_t)1 : (uint64_t)0;
      rlo = blo;
      rhi = bhi;
      carry = carry - b1;
      if (i >= 64)
        qhi |= (uint64_t)1 << (i - 64);
      else
        qlo |= (uint64_t)1 << i;
    }
  }
  *q = pli_u128(qlo, qhi);
  *r = pli_u128(rlo, rhi);
#else
  *q = n / d;
  *r = n % d;
#endif
}

/* Divide *v by 10 in place; return the remainder. Used for decimal output. */
static inline unsigned pli_divmod10(PliU128 *v) {
  PliU128 q, r;
  pli_divmod_u(*v, pli_u128(10u, 0u), &q, &r);
  *v = q;
#ifdef PLI_WIDE128_PORTABLE
  return (unsigned)r.lo;
#else
  return (unsigned)r;
#endif
}

/* --- signed sugar -------------------------------------------------------- */

static inline PliI128 pli_neg_s(PliI128 v) { return pli_to_s(pli_neg_u(pli_to_u(v))); }

/* Two's-complement magnitude without signed overflow (INT128_MIN-safe). */
static inline PliU128 pli_mag_s(PliI128 v) {
  return pli_isneg_s(v) ? pli_neg_u(pli_to_u(v)) : pli_to_u(v);
}

static inline PliI128 pli_sdiv(PliI128 a, PliI128 b) {
#ifdef PLI_WIDE128_PORTABLE
  /* Truncating division (C++/LLVM sdiv): divide magnitudes, fix the sign. */
  int neg = pli_isneg_s(a) ^ pli_isneg_s(b);
  PliU128 q, r;
  PliU128 mag;
  pli_divmod_u(pli_mag_s(a), pli_mag_s(b), &q, &r);
  mag = q;
  return neg ? pli_neg_s(pli_to_s(mag)) : pli_to_s(mag);
#else
  return a / b;
#endif
}

/* --- extraction / conversion --------------------------------------------- */

static inline uint64_t pli_slo64(PliI128 v) {
#ifdef PLI_WIDE128_PORTABLE
  return v.lo;
#else
  return (uint64_t)v;
#endif
}

static inline uint64_t pli_shi64(PliI128 v) {
#ifdef PLI_WIDE128_PORTABLE
  return (uint64_t)v.hi;
#else
  return (uint64_t)(v >> 64);
#endif
}

/* Low 64 bits (matches the historical `(long long)wideIval` truncation). */
static inline long long pli_to_i64(PliI128 v) {
#ifdef PLI_WIDE128_PORTABLE
  return (long long)v.lo;
#else
  return (long long)v;
#endif
}

#ifdef __cplusplus
#include <cstdlib> /* strtod for exactly-rounded pli_to_double */
#include <string>  /* std::string / std::reverse for pli_to_dec_string */

/* Exactly-rounded conversion to double: the decimal rendering is exact and
 * strtod rounds once (C locale digits only), matching hardware conversion. */
static inline double pli_to_double(PliI128 v) {
  bool neg = pli_isneg_s(v) != 0;
  PliU128 u = pli_mag_s(v);
  char buf[48];
  int n = 0;
  if (pli_iszero_u(u))
    buf[n++] = '0';
  while (!pli_iszero_u(u))
    buf[n++] = (char)('0' + pli_divmod10(&u));
  if (neg)
    buf[n++] = '-';
  for (int i = 0; i < n / 2; i++) {
    char t = buf[i];
    buf[i] = buf[n - 1 - i];
    buf[n - 1 - i] = t;
  }
  buf[n] = '\0';
  return strtod(buf, NULL);
}

/* Decimal rendering of a signed value (HIR `--print-hir`, diagnostics). */
static inline std::string pli_to_dec_string(PliI128 v) {
  if (pli_iszero_s(v))
    return "0";
  bool neg = pli_isneg_s(v) != 0;
  PliU128 u = pli_mag_s(v);
  std::string s;
  while (!pli_iszero_u(u))
    s.push_back((char)('0' + pli_divmod10(&u)));
  if (neg)
    s.push_back('-');
  for (size_t i = 0; i < s.size() / 2; i++) {
    char t = s[i];
    s[i] = s[s.size() - 1 - i];
    s[s.size() - 1 - i] = t;
  }
  return s;
}
#endif /* __cplusplus */

#ifdef __cplusplus
#ifdef PLI_WIDE128_PORTABLE
/* MSVC (portable fallback) operator sugar so compiler sources read like
 * native __int128 code. clang-cl uses native operators instead. */
 * Mixed int operands convert implicitly via PliI128(long long). */
inline PliI128 operator+(PliI128 a, PliI128 b) { return pli_to_s(pli_add_u(pli_to_u(a), pli_to_u(b))); }
inline PliI128 operator-(PliI128 a, PliI128 b) { return pli_to_s(pli_sub_u(pli_to_u(a), pli_to_u(b))); }
inline PliI128 operator-(PliI128 a) { return pli_neg_s(a); }
inline PliI128 operator*(PliI128 a, PliI128 b) { return pli_to_s(pli_mul_u(pli_to_u(a), pli_to_u(b))); }
inline PliI128 operator/(PliI128 a, PliI128 b) { return pli_sdiv(a, b); }
inline bool operator==(PliI128 a, PliI128 b) { return a.lo == b.lo && a.hi == b.hi; }
inline bool operator!=(PliI128 a, PliI128 b) { return !(a == b); }
inline bool operator<(PliI128 a, PliI128 b) {
  bool an = pli_isneg_s(a), bn = pli_isneg_s(b);
  if (an != bn)
    return an;
  /* Same sign: two's-complement bit patterns order like the signed values. */
  return pli_cmp_u(pli_to_u(a), pli_to_u(b)) < 0;
}
inline bool operator>(PliI128 a, PliI128 b) { return b < a; }
inline bool operator<=(PliI128 a, PliI128 b) { return !(b < a); }
inline bool operator>=(PliI128 a, PliI128 b) { return !(a < b); }
#endif /* PLI_WIDE128_PORTABLE */
#endif /* __cplusplus */

#endif /* PLI_WIDE128_H */
