/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* rt_edit.c — PL/I runtime library (pli_runtime): edit-directed PUT/GET. */
/* Split from pli_rt.c; pli_rt.h + pli_rt_abi.def stay the single ABI source. */
#include "pli_rt.h"
/* Output routes through rt_put_raw and pli_snprintf; no direct
 * stdio use remains, so no system header is included here. */

static void put_field(const char *p, size_t n, long long w) {
  if (w > (long long)n)
    for (long long i = n; i < w; ++i) rt_put_raw(" ", 1);
  rt_put_raw(p, n);
}

/* F(w,d) on a FIXED value stored as value*10^scale: rescale to exactly d
 * fractional digits (rounding half away from zero on a downward rescale), then
 * right-justify the result in width w. */
void pli_put_edit_fixed(long long v, long long scale, long long w, long long d) {
  if (d < 0) d = 0;
  if (d > 60) d = 60;
  long long sc = scale - d;
  if (sc > 0) { /* divide by 10^sc, rounding half away from zero */
    long long f = 1, t = sc;
    while (t--) f *= 10;
    int neg = v < 0;
    unsigned long long u = neg ? (unsigned long long)(-(v + 1)) + 1 : (unsigned long long)v;
    unsigned long long q = u / (unsigned long long)f, r = u % (unsigned long long)f;
    if (r * 2 >= (unsigned long long)f)
      ++q;
    v = neg ? -(long long)q : (long long)q;
  } else if (sc < 0) {
    long long f = 1, t = -sc;
    while (t--) f *= 10;
    v *= f;
  }
  char buf[96];
  char *p = buf;
  if (d > 0) {
    long long f = 1, t = d;
    while (t--) f *= 10;
    long long ip = v / f, fr = v % f;
    if (fr < 0) fr = -fr;
    int n = pli_snprintf(p, (size_t)(buf + sizeof buf - p), "%lld", ip);
    p += n;
    *p++ = '.';
    char frac[64];
    int fn = pli_snprintf(frac, sizeof frac, "%0*lld", (int)d, fr);
    for (int i = 0; i < fn; ++i) *p++ = frac[i];
  } else {
    int n = pli_snprintf(p, (size_t)(buf + sizeof buf - p), "%lld", v);
    p += n;
  }
  put_field(buf, (size_t)(p - buf), w);
}

/* F(w,d) on a FLOAT value: d fractional digits, right-justified in width w. */
void pli_put_edit_float(double v, long long w, long long d) {
  if (d < 0) d = 0;
  if (d > 60) d = 60;
  char buf[128];
  int n = pli_snprintf(buf, sizeof buf, "%.*f", (int)d, v);
  put_field(buf, (size_t)n, w);
}

/* E(w,d) on a FLOAT value: scientific notation with one leading digit, d
 * fractional digits, and a two-digit signed exponent (e.g. 1.25E+04), right-
 * justified in width w. `%.*e` yields `d.dddde±e`; the exponent is normalised
 * to at least two digits (0-padded) per the E-format convention. */
void pli_put_edit_float_e(double v, long long w, long long d) {
  if (d < 0) d = 0;
  if (d > 60) d = 60;
  char tmp[128];
  pli_snprintf(tmp, sizeof tmp, "%.*e", (int)d, v);
  char buf[128];
  char *p = buf;
  const char *q = tmp;
  if (*q == '-') { *p++ = *q++; }        /* sign */
  const char *dot = pli_strchr(q, 'e');
  size_t mant = (size_t)(dot ? dot - q : pli_strlen(q));
  pli_memcpy(p, q, mant); p += mant;          /* d.dddd mantissa */
  if (!dot) { *p = '\0'; put_field(buf, (size_t)(p - buf), w); return; }
  const char *e = dot + 1;                /* e.g. "+04" or "-12" */
  int ex = (int)pli_strtoll(e, NULL, 10);
  int neg = ex < 0;
  if (neg) ex = -ex;
  *p++ = 'E';
  *p++ = neg ? '-' : '+';
  if (ex < 10) *p++ = '0';
  p += pli_snprintf(p, (size_t)(buf + sizeof buf - p), "%d", ex);
  put_field(buf, (size_t)(p - buf), w);
}

/* A(w): a character value right-justified in width w, truncated on the right
 * when longer than the field. */
void pli_put_edit_char(const char *p, long long len, long long w) {
  long long take = len < w ? len : w;
  if (w > take)
    for (long long i = take; i < w; ++i) rt_put_raw(" ", 1);
  if (take > 0)
    rt_put_raw(p, (size_t)take);
}

/* X(w): w blank characters. */
void pli_put_edit_x(long long w) {
  for (long long i = 0; i < w; ++i) rt_put_raw(" ", 1);
}

/* COLUMN(n) (rule (48)): the next item starts at 1-based column n; pad
 * blanks, opening a fresh line first when already past n. n < 1 is 1. */
void pli_put_edit_column(long long n) {
  if (n < 1)
    n = 1;
  if (rt_col > 0 && rt_col + 1 > (int)n) {
    rt_put_raw("\n", 1);
    rt_col = 0;
  }
  while (rt_col + 1 < (int)n)
    rt_put_raw(" ", 1);
}

/* SKIP(n): advance to the start of a line and then n-1 further lines. */
void pli_put_edit_skip(long long n) {
  if (n < 1) n = 1;
  for (long long i = 0; i < n; ++i) rt_put_raw("\n", 1);
}

/* PAGE: a form feed. */
void pli_put_edit_page(void) { rt_put_raw("\f", 1); }

/* LINE(n): advance so the next item is on line n (1-based). A line counter is
 * not tracked, so this emits (n-1) newlines as an approximation. */
void pli_put_edit_line(long long n) {
  if (n < 1) n = 1;
  for (long long i = 1; i < n; ++i) rt_put_raw("\n", 1);
}

/* Edit-directed input (rules (108),(44)-(54)). */

/* F(w,d): read a fixed-width field of w characters and parse it as a number.
 * The caller converts the double to the target's type. */
double pli_get_edit_num(long long w) {
  char field[128];
  long long i = 0;
  for (; i < w && i < (long long)(sizeof field - 1); ++i) {
    int c = rt_next_char();
    if (c == EOF)
      break;
    field[i] = (char)c;
  }
  field[i] = '\0';
  return pli_strtod(field, NULL);
}

/* A(w): read w characters left-justified into dst, blank-padded to cap; a field
 * wider than the target is truncated (the excess input is still consumed). */
void pli_get_edit_char(char *dst, long long cap, long long w) {
  long long i = 0;
  for (; i < w; ++i) {
    int c = rt_next_char();
    if (c == EOF)
      break;
    if (i < cap)
      dst[i] = (char)c;
  }
  for (; i < cap; ++i)
    dst[i] = ' ';
}

/* X(w): skip w input characters. */
void pli_get_edit_x(long long w) {
  for (long long i = 0; i < w; ++i) rt_next_char();
}

/* B(w) output (rule (52)): nbits packed big-endian bits (first bit in the
 * top bit) written as '0'/'1', right-justified in width w. */
void pli_put_edit_bit(const char *p, long long nbits, long long w) {
  if (nbits < 0)
    nbits = 0;
  if (w < nbits)
    w = nbits;
  for (long long i = nbits; i < w; ++i)
    rt_put_raw(" ", 1);
  for (long long i = 0; i < nbits; ++i) {
    unsigned char byte = p ? (unsigned char)p[i / 8] : 0;
    int bit = (byte >> (7 - (i % 8))) & 1;
    rt_put_raw(bit ? "1" : "0", 1);
  }
}

/* B(w) input (rule (52)): read a w-character field, collect '0'/'1' (leading
 * blanks skipped), store up to cap bits packed big-endian; short fields are
 * '0'-padded on the right to match BIT literal semantics, excess consumed. */
void pli_get_edit_bit(char *dst, long long cap, long long w) {
  long long nbytes = cap > 0 ? (cap + 7) / 8 : 0;
  for (long long i = 0; i < nbytes; ++i)
    dst[i] = 0;
  if (w < 0)
    w = 0;
  long long got = 0;
  for (long long i = 0; i < w; ++i) {
    int c = rt_next_char();
    if (c == EOF)
      break;
    if (c != '0' && c != '1')
      continue; // blanks/padding skipped, like numeric fields
    if (got < cap)
      dst[got / 8] |= (char)((c - '0') << (7 - (got % 8)));
    ++got;
  }
}

/* COLUMN(n) on input (rule (48)): the next item starts at 1-based column n;
 * skip blanks forward, opening a fresh line first when already past n. */
void pli_get_edit_column(long long n) {
  if (n < 1)
    n = 1;
  if (rt_in_col + 1 > (int)n) {
    int c;
    do {
      c = rt_next_char();
      if (c == EOF)
        break;
    } while (c != '\n');
  }
  while (rt_in_col + 1 < (int)n) {
    int c = rt_next_char();
    if (c == EOF)
      break;
  }
}

/* SKIP(n): consume input up to and including the nth newline. */
void pli_get_edit_skip(long long n) {
  if (n < 1) n = 1;
  int nl = 0;
  while (nl < n) {
    int c = rt_next_char();
    if (c == EOF)
      break;
     if (c == '\n')
       ++nl;
  }
}

/* CALL ... EVENT(ev): mark the event incomplete before the task starts. */

/* P picture-directed output (rules (53),(146)-(148)): format a FIXED value
 * (stored as val*10^scale) against the PICTURE edit string. Digits are
 * right-aligned in digit positions; Z suppresses leading zeros, 9 forces a
 * zero. Supports: Z, 9, V, ., +, -, S, X, A, and fixed characters. */
void pli_put_edit_pic(long long val, long long scale, int sign,
                      const char *pic, long long piclen) {
  if (piclen <= 0)
    return;
  int isneg = sign < 0;
  long long av = isneg ? -val : val;

  /* Decompose |val| into big-endian decimal digits. */
  long long digits[64];
  long long ndig = 0;
  if (av == 0) {
    digits[0] = 0;
    ndig = 1;
  } else {
    long long tmp = av;
    while (tmp > 0 && ndig < 64) {
      digits[ndig++] = tmp % 10;
      tmp /= 10;
    }
    for (long long i = 0; i < ndig / 2; ++i) {
      long long t = digits[i];
      digits[i] = digits[ndig - 1 - i];
      digits[ndig - 1 - i] = t;
    }
  }

  /* Count digit positions and locate the decimal marker (V or first '.'). */
  long long vpos = -1;
  for (long long i = 0; i < piclen; ++i) {
    if (pic[i] == 'V' || (pic[i] == '.' && vpos < 0)) { vpos = i; break; }
  }
  /* Picture fractional digit count (digits after the decimal marker). */
  long long picfrac = 0;
  if (vpos >= 0)
    for (long long i = vpos + 1; i < piclen; ++i)
      if (pic[i] == 'Z' || pic[i] == '9') ++picfrac;

  /* Value: val_int integer digits, val_frac fractional digits. */
  long long val_int = ndig > scale ? ndig - scale : 0;
  long long val_frac = scale < ndig ? scale : 0;

  /* Walk the picture, emitting characters. */
  char outbuf[128];
  long long ocap = piclen < (long long)sizeof(outbuf) ? piclen : (long long)sizeof(outbuf);
  for (long long i = 0; i < ocap; ++i) outbuf[i] = ' ';
  long long oi = 0;
  int led_zero = 1;

  /* Track current picture digit index for fractional positions. */
  long long frac_idx = 0;  /* 0-based among picture fractional digit positions */

  for (long long pi = 0; pi < piclen && oi < ocap; ++pi) {
    char c = pic[pi];
    int is_fractional = (vpos >= 0 && pi > vpos);

    if (c == 'V') continue;  /* no output char */

    if (c == '9' || c == 'Z') {
      if (is_fractional) {
        /* Fractional digit: emit value's fractional digit or '0'. */
        long long src = val_int + frac_idx;
        long long vfrac_idx = frac_idx;  /* index among value fractional digits */
        if (vfrac_idx < val_frac) {
          char d = (char)('0' + digits[val_int + vfrac_idx]);
          led_zero = 0;
          outbuf[oi++] = d;
        } else {
          outbuf[oi++] = '0';  /* zero-pad if value has fewer fractional digits */
        }
        ++frac_idx;
      } else {
        /* Integer digit: right-align value integer digits in picture positions.
         * Count total picture integer digit positions. */
        long long pic_int_total = 0;
        for (long long j = 0; j < (vpos >= 0 ? vpos : piclen); ++j)
          if (pic[j] == 'Z' || pic[j] == '9') ++pic_int_total;
        /* Our index among picture integer positions (left-to-right). */
        long long int_idx = 0;
        for (long long j = 0; j < pi; ++j)
          if (pic[j] == 'Z' || pic[j] == '9') ++int_idx;
        /* Map to source digit index: right-aligned. */
        long long src_idx = int_idx - (pic_int_total - val_int);
        if (src_idx < 0) {
          outbuf[oi++] = (c == '9') ? '0' : ' ';  /* Z→blank, 9→zero */
        } else if (src_idx < val_int) {
          char d = (char)('0' + digits[src_idx]);
          if (c == 'Z' && d == '0' && led_zero) {
            outbuf[oi++] = ' ';  /* leading zero suppression */
          } else {
            led_zero = 0;
            outbuf[oi++] = d;
          }
        } else {
          /* Value has fewer integer digits than picture; Z→blank, 9→'0'. */
          outbuf[oi++] = (c == '9') ? '0' : ' ';
        }
      }
      continue;
    }
    if (c == '.' || c == ',') { outbuf[oi++] = '.'; continue; }
    if (c == '+' || c == '-') {
      outbuf[oi++] = isneg ? '-' : (c == '-' ? ' ' : '+');
      continue;
    }
    if (c == 'S') { outbuf[oi++] = isneg ? '-' : ' '; continue; }
    if (c == 'X') { outbuf[oi++] = ' '; continue; }
    if (c == 'A') { outbuf[oi++] = ' '; continue; }
    outbuf[oi++] = c;
  }
  outbuf[ocap] = '\0';
  rt_put_raw(outbuf, oi);
}

