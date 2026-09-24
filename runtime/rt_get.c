/* rt_get.c — PL/I runtime library (libpli): list/data-directed input. */
/* Split from pli_rt.c; pli_rt.h + pli_rt_abi.def stay the single ABI source. */
#include "pli_rt.h"
#include <stdio.h>

/* List-directed input (rule 109): read the next whitespace/comma-delimited
 * token from SYSIN (stdin) into buf (nul-terminated). A ';' also terminates
 * a token (for data-directed NAME=value pairs, rule (106)) and raises
 * rt_tok_semi for pli_get_data_next. Returns 0 at end of input. */
int rt_get_token(char *buf, size_t cap) {
  int c;
  do {
    c = rt_next_char();
  } while (c != EOF && (c == ' ' || c == '\t' || c == '\n' || c == '\r'));
  if (c == EOF)
    return 0;
  size_t n = 0;
  while (c != EOF && c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != ',' && c != ';') {
    if (n + 1 < cap)
      buf[n++] = (char)c;
    c = rt_next_char();
  }
  buf[n] = '\0';
  if (c == ';')
    rt_tok_semi = 1;
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
  get_token(tok, sizeof tok);
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
  for (;;) {
    if (rt_tok_semi) {
      rt_tok_semi = 0;
      return 0;
    }
    int c;
    do {
      c = rt_next_char();
    } while (c != EOF && (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ','));
    if (c == EOF || c == ';')
      return 0;
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
      if (c == ';')
        return 0;
      continue;
    }
    if (n == 0) {
      // Bare '=value': discard the value and retry.
      char tok[256];
      get_token(tok, sizeof tok);
      continue;
    }
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
  return neg ? -(long long)acc : (long long)acc;
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
      while (*t >= '0' && *t <= '9')
        e10 = e10 * 10 + (*t++ - '0');
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

