/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* rt_get.c — PL/I runtime library (pli_runtime): list/data-directed input. */
/* Split from pli_rt.c; pli_rt.h + pli_rt_abi.def stay the single ABI source. */
#include "pli_rt.h"
/* EOF/size_t arrive via the platform seam headers; input itself routes
 * through rt_next_char, so no stdio calls remain here. */

/* List-directed input (rule 109): read the next whitespace/comma-delimited
 * token from SYSIN (stdin) into buf (nul-terminated). A ';' also terminates
 * a token (for data-directed NAME=value pairs, rule (106)) and raises
 * rt_tok_semi for pli_get_data_next. Returns 0 at end of input.
 * The input lock (ADR-233) is taken here: the token and the column state it
 * advances are read as a unit, so two tasks do not split one token. */
int rt_get_token(char *buf, size_t cap) {
  rt_in_lock();
  int c;
  do {
    c = rt_next_char();
  } while (c != EOF && (c == ' ' || c == '\t' || c == '\n' || c == '\r'));
  if (c == EOF) {
    rt_in_unlock();
    return 0;
  }
  /* A zero or negative capacity is a caller bug: the cap comparison would
   * wrap and the terminator would land outside the buffer. */
  if (cap <= 0) {
    rt_in_unlock();
    return 0;
  }
  size_t n = 0;
  while (c != EOF && c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != ',' && c != ';') {
    if (n + 1 < cap)
      buf[n++] = (char)c;
    c = rt_next_char();
  }
  buf[n] = '\0';
  if (c == ';')
    rt_tok_semi = 1;
  rt_in_unlock();
  return 1;
}

long long pli_get_list_fixed(void) {
  char tok[64];
  if (!get_token(tok, sizeof tok))
    return 0;
  return pli_strtoll(tok, NULL, 10);
}

double pli_get_list_float(void) {
  char tok[64];
  if (!get_token(tok, sizeof tok))
    return 0.0;
  return pli_strtod(tok, NULL);
}

void pli_get_list_char(char *dst, long long cap) {
  char tok[256];
  /* get_token leaves the buffer untouched at EOF: check its result, or the
   * copy below reads uninitialised stack. */
  if (!get_token(tok, sizeof tok)) {
    for (long long i = 0; i < cap; ++i)
      dst[i] = ' ';
    return;
  }
  long long i = 0;
  for (; i < cap && tok[i]; ++i)
    dst[i] = tok[i];
  for (; i < cap; ++i)
    dst[i] = ' ';
}

/* FIXED DECIMAL input (QR1.2): scale the token by 10^q, truncating extra
 * fraction digits; lenient like the other readers. */
long long pli_get_list_decfixed(long long q) {
  char tok[64];
  if (!get_token(tok, sizeof tok))
    return 0;
  int neg = 0;
  const char *p = tok;
  if (*p == '+' || *p == '-') {
    neg = *p == '-';
    ++p;
  }
  long long whole = 0;
  while (*p >= '0' && *p <= '9')
    whole = whole * 10 + (*p++ - '0');
  long long frac = 0;
  long long qd = 0;
  if (*p == '.') {
    ++p;
    while (*p >= '0' && *p <= '9' && qd < q) {
      frac = frac * 10 + (*p++ - '0');
      ++qd;
    }
    while (*p >= '0' && *p <= '9')
      ++p; // truncate beyond q
  }
  while (qd++ < q)
    frac *= 10;
  long long v = whole;
  for (long long i = 0; i < q; ++i)
    v *= 10;
  v += frac;
  return neg ? -v : v;
}

/* Wide FIXED DECIMAL input (B1): the same token rules as
 * pli_get_list_decfixed, returned as a 128-bit value through `out`. */
void pli_get_list_decfixed_wide(long long q, char *out) {
  PliI128 *dst = (PliI128 *)out;
  char tok[64];
  if (!get_token(tok, sizeof tok)) {
    *dst = pli_from_i64(0);
    return;
  }
  if (q < 0)
    q = 0;
  if (q > 38)
    q = 38;
  int neg = 0;
  const char *p = tok;
  if (*p == '+' || *p == '-') {
    neg = *p == '-';
    ++p;
  }
  PliU128 ten = pli_u128(10u, 0u);
  PliU128 whole = pli_u128(0u, 0u);
  while (*p >= '0' && *p <= '9')
    whole = pli_add_u(pli_mul_u(whole, ten), pli_u128((uint64_t)(*p++ - '0'), 0u));
  PliU128 frac = pli_u128(0u, 0u);
  long long qd = 0;
  if (*p == '.') {
    ++p;
    while (*p >= '0' && *p <= '9' && qd < q) {
      frac = pli_add_u(pli_mul_u(frac, ten), pli_u128((uint64_t)(*p++ - '0'), 0u));
      ++qd;
    }
    while (*p >= '0' && *p <= '9')
      ++p; // truncate beyond q
  }
  while (qd++ < q)
    frac = pli_mul_u(frac, ten);
  for (long long i = 0; i < q; ++i)
    whole = pli_mul_u(whole, ten);
  PliI128 v = pli_to_s(pli_add_u(whole, frac));
  *dst = neg ? pli_neg_s(v) : v;
}

/* FIXED BINARY input (ADR-284): parse a decimal token into value*2^q,
 * truncating extra fraction digits. At most 18 significant digits are
 * kept; lenient like the other readers. */
long long pli_get_list_fixed_scaled(long long q) {
  char tok[64];
  if (!get_token(tok, sizeof tok))
    return 0;
  if (q < 0)
    q = 0;
  if (q > 62)
    q = 62;
  int neg = 0;
  const char *p = tok;
  if (*p == '+' || *p == '-') {
    neg = *p == '-';
    ++p;
  }
  PliU128 num = pli_u128(0u, 0u);
  PliU128 ten = pli_u128(10u, 0u);
  int digits = 0;
  int frac = 0;
  while (*p >= '0' && *p <= '9') {
    if (digits < 18) {
      num = pli_add_u(pli_mul_u(num, ten), pli_u128((uint64_t)(*p - '0'), 0u));
      ++digits;
    }
    ++p;
  }
  if (*p == '.') {
    ++p;
    while (*p >= '0' && *p <= '9') {
      if (digits < 18) {
        num = pli_add_u(pli_mul_u(num, ten), pli_u128((uint64_t)(*p - '0'), 0u));
        ++digits;
        ++frac;
      }
      ++p;
    }
  }
  /* target raw = num * 2^q / 10^frac, truncated toward zero */
  PliU128 pow2 = pli_u128(1u, 0u);
  PliU128 two = pli_u128(2u, 0u);
  for (long long i = 0; i < q; ++i)
    pow2 = pli_mul_u(pow2, two);
  PliU128 den = pli_u128(1u, 0u);
  for (int i = 0; i < frac; ++i)
    den = pli_mul_u(den, ten);
  PliU128 quo, rem;
  pli_divmod_u(pli_mul_u(num, pow2), den, &quo, &rem);
  unsigned long long mag;
#ifdef PLI_WIDE128_PORTABLE
  mag = quo.hi != 0 ? 0x7fffffffffffffffULL : quo.lo;
#else
  mag = (unsigned long long)quo > 0x7fffffffffffffffULL
            ? 0x7fffffffffffffffULL
            : (unsigned long long)quo;
#endif
  return neg ? -(long long)mag : (long long)mag;
}

/* List-directed complex input (CM5, ADR-086): a `re+imI` token reads both
 * parts; a bare number reads with a zero imaginary part. Lenient like the
 * other readers: missing or malformed parts read as zero. */
void pli_get_list_complex(char *re_ptr, char *im_ptr) {
  // The ABI token set has no double-pointer: the caller passes double* for
  // both (INCITS parity aside, the effective type stays double throughout).
  double *re = (double *)re_ptr;
  double *im = (double *)im_ptr;
  char tok[128];
  if (!get_token(tok, sizeof tok)) {
    *re = 0.0;
    *im = 0.0;
    return;
  }
  size_t n = pli_strlen(tok);
  if (n > 0 && (tok[n - 1] == 'I' || tok[n - 1] == 'i')) {
    tok[n - 1] = '\0';
    // Split at the last interior sign that is not an exponent marker.
    size_t k = pli_strlen(tok);
    size_t split = 0;
    for (size_t j = 1; j < k; ++j)
      if ((tok[j] == '+' || tok[j] == '-') && tok[j - 1] != 'e' && tok[j - 1] != 'E')
        split = j;
    if (split == 0) {
      *re = 0.0;
      *im = pli_strtod(tok, NULL);
    } else {
      *re = pli_strtod(tok, NULL);
      *im = pli_strtod(tok + split, NULL);
    }
  } else {
    *re = pli_strtod(tok, NULL);
    *im = 0.0;
  }
}

unsigned char pli_get_list_bit(void) {
  char tok[16];
  if (!get_token(tok, sizeof tok))
    return 0;
  return tok[0] == '1' ? 1 : 0;
}

/* Data-directed input (rule (106), QR1.5): read the next NAME= pair head.
 * Returns the uppercased name length (0 at ';'/EOF, consuming the ';');
 * malformed pairs without '=' are skipped. The value itself stays queued
 * for the typed list-directed reader (get_token stops at ';', raising
 * rt_tok_semi for the following call). Lenient like the other readers. */
int rt_data_namechar(int c) {
  return pli_isalnum(c) || c == '_' || c == '$' || c == '#';
}

long long pli_get_data_next(char *buf, long long cap) {
  rt_in_lock();
  for (;;) {
    if (rt_tok_semi) {
      rt_tok_semi = 0;
      rt_in_unlock();
      return 0;
    }
    int c;
    do {
      c = rt_next_char();
    } while (c != EOF && (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ','));
    if (c == EOF || c == ';') {
      rt_in_unlock();
      return 0;
    }
    size_t n = 0;
    while (c != EOF && data_namechar(c)) {
      if (n + 1 < (size_t)cap)
        buf[n++] = (char)pli_toupper(c);
      c = rt_next_char();
    }
    buf[n] = '\0';
    while (c == ' ' || c == '\t')
      c = rt_next_char();
    if (c != '=') {
      // Malformed pair without '=': skip to the next pair and retry.
      while (c != EOF && c != ',' && c != ';' && c != '\n')
        c = rt_next_char();
      if (c == ';') {
        rt_in_unlock();
        return 0;
      }
      continue;
    }
    if (n == 0) {
      // Bare '=value': discard the value and retry.
      char tok[256];
      get_token(tok, sizeof tok);
      continue;
    }
    rt_in_unlock();
    return (long long)n;
  }
}

/* Discard one data-directed value (unknown NAME): the token queued by
 * pli_get_data_next. */
void pli_get_data_skip(void) {
  char tok[256];
  get_token(tok, sizeof tok);
}

/* Compare an input NAME against an expected variable name. */
int pli_data_name_is(const char *p, long long n, const char *q, long long m) {
  return n == m && pli_memcmp(p, q, (size_t)n) == 0;
}

/* ---------------------------------------------------------------------------
 * Ported number parsing (runtime-reduction plan, Phase 2).
 *
 * Base-10-only, ASCII-only, no hex/octal/inf/nan, no locale. Clamp on
 * overflow for the integer form (matches the original strtoll callers, which
 * passed base 10 and NULL endptr). These are internal helpers, not ABI.
 * ------------------------------------------------------------------------- */

long long pli_strtoll(const char *nptr, char **endptr, int base) {
  (void)base; /* base-10 only */
  const char *s = nptr;
  while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
    ++s;
  int neg = 0;
  if (*s == '+' || *s == '-') {
    neg = *s == '-';
    ++s;
  }
  unsigned long long acc = 0;
  int any = 0;
  while (*s >= '0' && *s <= '9') {
    unsigned d = (unsigned)(*s++ - '0');
    if (acc > (0xFFFFFFFFFFFFFFFFULL - d) / 10) {
      acc = neg ? 0x8000000000000000ULL : 0xFFFFFFFFFFFFFFFFULL;
      any = 1;
      break;
    }
    acc = acc * 10 + d;
    any = 1;
  }
  if (endptr)
    *endptr = (char *)(any ? s : nptr);
  if (!any)
    return 0;
  /* The negation is done in unsigned arithmetic: negating LLONG_MIN as a
   * signed value overflows. */
  return neg ? (long long)(0ULL - acc) : (long long)acc;
}

double pli_strtod(const char *nptr, char **endptr) {
  const char *s = nptr;
  while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
    ++s;
  int neg = 0;
  if (*s == '+' || *s == '-') {
    neg = *s == '-';
    ++s;
  }
  double val = 0.0;
  int seen = 0;
  while (*s >= '0' && *s <= '9') {
    val = val * 10.0 + (double)(*s++ - '0');
    seen = 1;
  }
  if (*s == '.') {
    ++s;
    double scale = 1.0;
    while (*s >= '0' && *s <= '9') {
      val = val * 10.0 + (double)(*s++ - '0');
      scale *= 10.0;
      seen = 1;
    }
    val /= scale;
  }
  if (seen && (*s == 'e' || *s == 'E')) {
    const char *t = s + 1;
    int eneg = 0;
    if (*t == '+' || *t == '-') {
      eneg = *t == '-';
      ++t;
    }
    if (*t >= '0' && *t <= '9') {
      long long e10 = 0;
      /* Bound the exponent: the digit loop below runs once per power of ten,
       * so an unbounded exponent (1e99999999999999999999) would spin for
       * ever, and e10 * 10 overflows a long long. */
      while (*t >= '0' && *t <= '9' && e10 <= 400)
        e10 = e10 * 10 + (*t++ - '0');
      while (*t >= '0' && *t <= '9')
        ++t;
      if (eneg)
        e10 = -e10;
      s = t;
      while (e10 > 0) {
        val *= 10.0;
        --e10;
      }
      while (e10 < 0) {
        val /= 10.0;
        ++e10;
      }
    }
  }
  if (endptr)
    *endptr = (char *)(seen ? s : nptr);
  return neg ? -val : val;
}

/* STRING (rule 105) PUT: route list-directed output into buf (cap bytes). */

