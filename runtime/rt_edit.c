/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* rt_edit.c — PL/I runtime library (libpli): edit-directed PUT/GET. */
/* Split from pli_rt.c; pli_rt.h + pli_rt_abi.def stay the single ABI source. */
#include "pli_rt.h"
#include <stdio.h>

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

