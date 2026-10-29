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

/* F(w,d) on a FIXED value stored as value*10^scale: print the value with
 * exactly d fractional digits (rounding half away from zero on a downward
 * rescale), right-justified in width w.
 *
 * The stored integer is split into its integer part and its `scale` fraction
 * digits instead of being rescaled to 10^d: a picture may ask for more digits
 * than the scale holds (10^d does not fit an i64 past d = 18), and forming
 * 10^scale * 10^(d-scale) overflowed the value. */
void pli_put_edit_fixed(long long v, long long scale, long long w, long long d) {
  if (d < 0) d = 0;
  if (d > 60) d = 60;
  if (scale < 0) scale = 0;
  rt_out_lock();
  /* 10^scale: the largest power of ten an i64 holds is 10^18. */
  unsigned long long sf = 1;
  for (long long i = 0; i < scale && i < 18; ++i) sf *= 10;
  int neg = v < 0;
  unsigned long long u = neg ? (unsigned long long)(-(v + 1)) + 1 : (unsigned long long)v;
  unsigned long long ip = u / sf;
  unsigned long long fr = u % sf;
  /* Round the fraction down to d digits, half away from zero. */
  if (d < scale) {
    unsigned long long df = 1; /* 10^(scale-d) */
    for (long long i = 0; i < scale - d; ++i) df *= 10;
    unsigned long long ten_d = 1; /* 10^d */
    for (long long i = 0; i < d; ++i) ten_d *= 10;
    unsigned long long q = fr / df, r = fr % df;
    if (r * 2 >= df) ++q;
    if (q >= ten_d) { /* the rounding carried into the integer part */
      q = 0;
      ++ip;
    }
    fr = q;
  }
  char buf[128];
  char *p = buf;
  size_t room = sizeof buf;
  if (neg && (ip || fr)) {
    *p++ = '-';
    --room;
  }
  {
    char digits[24];
    int n = 0;
    unsigned long long t = ip;
    do {
      digits[n++] = (char)('0' + t % 10);
      t /= 10;
    } while (t);
    while (n && room > 1) {
      *p++ = digits[--n];
      --room;
    }
  }
  if (d > 0 && room > 1) {
    *p++ = '.';
    --room;
    /* Exactly d fraction digits: the value's own scale digits, zero-padded. */
    char frac[64];
    long long ndig = d < (long long)sizeof frac ? d : (long long)sizeof frac;
    for (long long i = 0; i < ndig; ++i) frac[i] = '0';
    long long nfrom = scale < ndig ? scale : ndig;
    unsigned long long t = fr;
    /* The value's own fraction digits come first (most significant); the
     * rest of the requested digits are zeros. */
    unsigned long long df = 1;
    for (long long i = 0; i + 1 < nfrom; ++i)
      df *= 10;
    for (long long i = 0; i < nfrom; ++i) {
      frac[i] = (char)('0' + (t / df) % 10);
      df /= 10;
    }
    for (long long i = 0; i < ndig && room > 1; ++i) {
      *p++ = frac[i];
      --room;
    }
  }
  *p = '\0';
  put_field(buf, (size_t)(p - buf), w);
  rt_out_unlock();
}

/* F(w,d) on a FLOAT value: d fractional digits, right-justified in width w. */
void pli_put_edit_float(double v, long long w, long long d) {
  if (d < 0) d = 0;
  if (d > 60) d = 60;
  char buf[128];
  int n = pli_snprintf(buf, sizeof buf, "%.*f", (int)d, v);
  rt_out_lock();
  put_field(buf, (size_t)n, w);
  rt_out_unlock();
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
  rt_out_lock();
  if (*q == '-') { *p++ = *q++; }        /* sign */
  const char *dot = pli_strchr(q, 'e');
  size_t mant = (size_t)(dot ? dot - q : pli_strlen(q));
  pli_memcpy(p, q, mant); p += mant;          /* d.dddd mantissa */
  if (!dot) {
    *p = '\0';
    put_field(buf, (size_t)(p - buf), w);
    rt_out_unlock();
    return;
  }
  const char *e = dot + 1;                /* e.g. "+04" or "-12" */
  int ex = (int)pli_strtoll(e, NULL, 10);
  int neg = ex < 0;
  if (neg) ex = -ex;
  *p++ = 'E';
  *p++ = neg ? '-' : '+';
  if (ex < 10) *p++ = '0';
  p += pli_snprintf(p, (size_t)(buf + sizeof buf - p), "%d", ex);
  put_field(buf, (size_t)(p - buf), w);
  rt_out_unlock();
}

/* A(w): a character value right-justified in width w, truncated on the right
 * when longer than the field. */
void pli_put_edit_char(const char *p, long long len, long long w) {
  long long take = len < w ? len : w;
  rt_out_lock();
  if (w > take)
    for (long long i = take; i < w; ++i) rt_put_raw(" ", 1);
  if (take > 0)
    rt_put_raw(p, (size_t)take);
  rt_out_unlock();
}

/* X(w): w blank characters. */
void pli_put_edit_x(long long w) {
  rt_out_lock();
  for (long long i = 0; i < w; ++i) rt_put_raw(" ", 1);
  rt_out_unlock();
}

/* COLUMN(n) (rule (48)): the next item starts at 1-based column n; pad
 * blanks, opening a fresh line first when already past n. n < 1 is 1. */
void pli_put_edit_column(long long n) {
  if (n < 1)
    n = 1;
  rt_out_lock();
  if (rt_col > 0 && rt_col + 1 > (int)n) {
    rt_put_raw("\n", 1);
    rt_col = 0;
  }
  while (rt_col + 1 < (int)n)
    rt_put_raw(" ", 1);
  rt_out_unlock();
}

/* SKIP(n): advance to the start of a line and then n-1 further lines. */
void pli_put_edit_skip(long long n) {
  if (n < 1) n = 1;
  rt_out_lock();
  for (long long i = 0; i < n; ++i) rt_put_raw("\n", 1);
  rt_out_unlock();
}

/* PAGE: a form feed. */
void pli_put_edit_page(void) {
  rt_out_lock();
  rt_put_raw("\f", 1);
  rt_out_unlock();
}

/* LINE(n): advance so the next item is on line n (1-based). A line counter is
 * not tracked, so this emits (n-1) newlines as an approximation. */
void pli_put_edit_line(long long n) {
  if (n < 1) n = 1;
  rt_out_lock();
  for (long long i = 1; i < n; ++i) rt_put_raw("\n", 1);
  rt_out_unlock();
}

/* Edit-directed input (rules (108),(44)-(54)). The readers take the input
 * lock (ADR-233): the field they read and the column state it advances
 * belong together, so two tasks do not split one field. */

/* F(w,d): read a fixed-width field of w characters and parse it as a number.
 * The caller converts the double to the target's type. */
double pli_get_edit_num(long long w) {
  char field[128];
  long long i = 0;
  rt_in_lock();
  for (; i < w && i < (long long)(sizeof field - 1); ++i) {
    int c = rt_next_char();
    if (c == EOF)
      break;
    field[i] = (char)c;
  }
  field[i] = '\0';
  rt_in_unlock();
  return pli_strtod(field, NULL);
}

/* P(picture) input (rules (53),(146)-(148), ADR-291): read a w-character
 * field and return its value scaled by the picture's assumed decimal point.
 * A blank, comma, currency symbol, '+' or '-' is skipped; a '-' anywhere sets
 * the sign, and a '.' moves the following digits to the fractional side.
 * Without a '.', every digit is already in picture positions (so a 9V99 field
 * "150" is 1.50 and comes back as the integer 150 at scale 2). */
long long pli_get_edit_pic(long long w, long long scale) {
  char field[128];
  long long i = 0;
  rt_in_lock();
  for (; i < w && i < (long long)(sizeof field - 1); ++i) {
    int c = rt_next_char();
    if (c == EOF)
      break;
    field[i] = (char)c;
  }
  field[i] = '\0';
  rt_in_unlock();
  long long nread = i;

  long long ival = 0, fval = 0, fcount = 0;
  int isneg = 0, has_point = 0;
  for (i = 0; i < nread; ++i) {
    char c = field[i];
    if (c >= '0' && c <= '9') {
      if (has_point) {
        if (fcount < scale) {
          fval = fval * 10 + (c - '0');
          ++fcount;
        }
      } else {
        ival = ival * 10 + (c - '0');
      }
    } else if (c == '.') {
      has_point = 1;
    } else if (c == '-') {
      isneg = 1;
    } else if (c == '+') {
      isneg = 0;
    }
    /* blanks, commas and currency signs carry no value */
  }
  long long val;
  if (has_point) {
    while (fcount < scale) {
      fval *= 10;
      ++fcount;
    }
    for (long long k = 0; k < scale; ++k)
      ival *= 10;
    val = ival + fval;
  } else {
    val = ival;
  }
  return isneg ? -val : val;
}

/* Wide P input (B1): the same field rules as pli_get_edit_pic, but the
 * scaled value is 128-bit. The result leaves through `out` because a
 * by-value 128-bit return lowers differently across targets. */
static PliU128 pic_mul10(PliU128 v, long long n) {
  PliU128 ten = pli_u128(10u, 0u);
  while (n-- > 0)
    v = pli_mul_u(v, ten);
  return v;
}

void pli_get_edit_pic_wide(long long w, long long scale, char *out) {
  char field[128];
  long long i = 0;
  PliI128 *dst = (PliI128 *)out;
  rt_in_lock();
  for (; i < w && i < (long long)(sizeof field - 1); ++i) {
    int c = rt_next_char();
    if (c == EOF)
      break;
    field[i] = (char)c;
  }
  field[i] = '\0';
  rt_in_unlock();
  long long nread = i;

  PliU128 ival = pli_u128(0u, 0u), fval = pli_u128(0u, 0u);
  PliU128 ten = pli_u128(10u, 0u);
  long long fcount = 0;
  int isneg = 0, has_point = 0;
  for (i = 0; i < nread; ++i) {
    char c = field[i];
    if (c >= '0' && c <= '9') {
      PliU128 d = pli_u128((uint64_t)(c - '0'), 0u);
      if (has_point) {
        if (fcount < scale) {
          fval = pli_add_u(pli_mul_u(fval, ten), d);
          ++fcount;
        }
      } else {
        ival = pli_add_u(pli_mul_u(ival, ten), d);
      }
    } else if (c == '.') {
      has_point = 1;
    } else if (c == '-') {
      isneg = 1;
    } else if (c == '+') {
      isneg = 0;
    }
    /* blanks, commas and currency signs carry no value */
  }
  if (has_point) {
    fval = pic_mul10(fval, scale - fcount);
    ival = pic_mul10(ival, scale);
  }
  PliU128 val = has_point ? pli_add_u(ival, fval) : ival;
  PliI128 sv = pli_to_s(val);
  *dst = isneg ? pli_neg_s(sv) : sv;
}

/* A(w): read w characters left-justified into dst, blank-padded to cap; a field
 * wider than the target is truncated (the excess input is still consumed).
 * Returns the assigned length: a VARYING target stores it, a fixed target
 * ignores it and stays blank-padded. */
long long pli_get_edit_char(char *dst, long long cap, long long w) {
  long long i = 0;
  rt_in_lock();
  for (; i < w; ++i) {
    int c = rt_next_char();
    if (c == EOF)
      break;
    if (i < cap)
      dst[i] = (char)c;
  }
  /* Save the read count before the padding loop drives i up to cap. */
  long long n = i < cap ? i : cap;
  for (; i < cap; ++i)
    dst[i] = ' ';
  rt_in_unlock();
  return n;
}

/* X(w): skip w input characters. */
void pli_get_edit_x(long long w) {
  rt_in_lock();
  for (long long i = 0; i < w; ++i) rt_next_char();
  rt_in_unlock();
}

/* B(w) output (rule (52)): nbits packed big-endian bits (first bit in the
 * top bit) written as '0'/'1', right-justified in width w. */
void pli_put_edit_bit(const char *p, long long nbits, long long w) {
  if (nbits < 0)
    nbits = 0;
  if (w < nbits)
    w = nbits;
  rt_out_lock();
  for (long long i = nbits; i < w; ++i)
    rt_put_raw(" ", 1);
  for (long long i = 0; i < nbits; ++i) {
    unsigned char byte = p ? (unsigned char)p[i / 8] : 0;
    int bit = (byte >> (7 - (i % 8))) & 1;
    rt_put_raw(bit ? "1" : "0", 1);
  }
  rt_out_unlock();
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
  rt_in_lock();
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
  rt_in_unlock();
}

/* COLUMN(n) on input (rule (48)): the next item starts at 1-based column n;
 * skip blanks forward, opening a fresh line first when already past n. */
void pli_get_edit_column(long long n) {
  if (n < 1)
    n = 1;
  rt_in_lock();
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
  rt_in_unlock();
}

/* SKIP(n): consume input up to and including the nth newline. */
void pli_get_edit_skip(long long n) {
  if (n < 1) n = 1;
  int nl = 0;
  rt_in_lock();
  while (nl < n) {
    int c = rt_next_char();
    if (c == EOF)
      break;
     if (c == '\n')
       ++nl;
  }
  rt_in_unlock();
}

/* CALL ... EVENT(ev): mark the event incomplete before the task starts. */

/* P picture-directed output (rules (53),(146)-(148)): format a FIXED value
 * (stored as val*10^scale) against the PICTURE edit string. Digits are
 * right-aligned in digit positions; Z suppresses leading zeros, 9 forces a
 * zero. Supports: Z, 9, V, ., +, -, S, X, A, and fixed characters. */
/* Picture formatter core (rules (53),(146)-(148)): writes at most `cap`
 * characters to `out` and returns the number written. The output routine and
 * a PICTURE variable assignment share it (ADR-290), so both sides cannot
 * disagree about the edited text. */
static long long pic_format128(PliI128 val, long long scale, int sign,
                               const char *pic, long long piclen,
                               char *out, long long cap) {
  if (piclen <= 0 || cap <= 0)
    return 0;
  int isneg = sign < 0;
  PliU128 av = isneg ? pli_mag_s(val) : pli_to_u(val);

  /* Decompose |val| into big-endian decimal digits (38 fit PliI128). */
  long long digits[64];
  long long ndig = 0;
  if (pli_iszero_u(av)) {
    digits[0] = 0;
    ndig = 1;
  } else {
    while (!pli_iszero_u(av) && ndig < 64)
      digits[ndig++] = (long long)pli_divmod10(&av);
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

  /* Value: val_int integer digits, val_frac fractional digits. */
  long long val_int = ndig > scale ? ndig - scale : 0;
  long long val_frac = scale < ndig ? scale : ndig;

  /* Walk the picture, emitting characters. A picture with a V position has
   * one more template character than output positions; cap is the output
   * width, so V simply does not consume a slot. */
  long long ocap = piclen < cap ? piclen : cap;
  for (long long i = 0; i < ocap; ++i) out[i] = ' ';
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
        long long vfrac_idx = frac_idx;  /* index among value fractional digits */
        if (vfrac_idx < val_frac) {
          char d = (char)('0' + digits[val_int + vfrac_idx]);
          led_zero = 0;
          out[oi++] = d;
        } else {
          out[oi++] = '0';  /* zero-pad if value has fewer fractional digits */
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
          out[oi++] = (c == '9') ? '0' : ' ';  /* Z→blank, 9→zero */
        } else if (src_idx < val_int) {
          char d = (char)('0' + digits[src_idx]);
          if (c == 'Z' && d == '0' && led_zero) {
            out[oi++] = ' ';  /* leading zero suppression */
          } else {
            led_zero = 0;
            out[oi++] = d;
          }
        } else {
          /* Value has fewer integer digits than picture; Z→blank, 9→'0'. */
          out[oi++] = (c == '9') ? '0' : ' ';
        }
      }
      continue;
    }
    if (c == '.' || c == ',') { out[oi++] = '.'; continue; }
    if (c == '+' || c == '-') {
      out[oi++] = isneg ? '-' : (c == '-' ? ' ' : '+');
      continue;
    }
    if (c == 'S') { out[oi++] = isneg ? '-' : ' '; continue; }
    if (c == 'X') { out[oi++] = ' '; continue; }
    if (c == 'A') { out[oi++] = ' '; continue; }
    out[oi++] = c;
  }
  return oi;
}

/* Sterling pictures (rules (19),(146)-(148)): the value is fixed-point pence.
 * G and the M subfield markers emit nothing; the pounds, shillings and pence
 * fields are filled from the three quantities. This slice accepts 9/Z digit
 * positions and . B / H P literals; other sterling editing is diagnosed by
 * the classifier and never reaches here. */
static void sterling_field(const char *pic, long long start, long long end,
                           long long value, char *out, long long cap, long long *oi,
                           int *seen) {
  char digits[128];
  long long n = 0;
  for (long long i = start; i < end; ++i)
    if (pic[i] == '9' || pic[i] == 'Z')
      ++n;
  if (n > (long long)sizeof digits)
    n = (long long)sizeof digits;
  for (long long i = 0; i < n; ++i)
    digits[i] = '0';
  long long v = value;
  for (long long i = n - 1; i >= 0 && v > 0; --i) {
    digits[i] = (char)('0' + v % 10);
    v /= 10;
  }
  long long k = 0;
  for (long long i = start; i < end && *oi < cap; ++i) {
    char c = pic[i];
    if (c == '9' || c == 'Z') {
      char d = digits[k++];
      if (c == 'Z' && d == '0' && !*seen) {
        out[(*oi)++] = ' ';
      } else {
        out[(*oi)++] = d;
        if (d != '0')
          *seen = 1;
      }
    } else if (c == '.') {
      out[(*oi)++] = '.';
    } else if (c == '/') {
      out[(*oi)++] = '/';
    } else if (c == 'B') {
      out[(*oi)++] = ' ';
    } else if (c == 'H') {
      out[(*oi)++] = 'S';
    } else if (c == 'P') {
      out[(*oi)++] = 'D';
    }
  }
}

/* Read one sterling subfield: each 9/Z position consumes one character and
 * contributes a decimal digit (blank counts as zero); literals are consumed
 * without a value. */
static long long sterling_read_field(const char *pic, long long start, long long end,
                                     const char *field, long long *pos) {
  long long value = 0;
  for (long long i = start; i < end; ++i) {
    char c = pic[i];
    if (c == '9' || c == 'Z') {
      char d = field[(*pos)++];
      if (d >= '0' && d <= '9')
        value = value * 10 + (d - '0');
    } else {
      ++*pos; // a literal occupies one field position
    }
  }
  return value;
}

static long long sterling_format(long long val, int sign, const char *pic, long long piclen,
                                 char *out, long long cap) {
  if (piclen <= 0 || cap <= 0)
    return 0;
  long long m1 = -1, m2 = -1, m3 = -1;
  for (long long i = 0; i < piclen; ++i)
    if (pic[i] == 'M') {
      if (m1 < 0)
        m1 = i;
      else if (m2 < 0)
        m2 = i;
      else if (m3 < 0)
        m3 = i;
    }
  if (m1 < 0 || m2 < 0 || m3 < 0)
    return 0;
  unsigned long long mag = sign < 0 ? (unsigned long long)(-(val + 1)) + 1u
                                    : (unsigned long long)val;
  unsigned long long pounds = mag / 240u;
  unsigned long long rem = mag % 240u;
  long long shill = (long long)(rem / 12u);
  long long pence = (long long)(rem % 12u);
  long long oi = 0;
  int seen = 0;
  sterling_field(pic, m1 + 1, m2, (long long)pounds, out, cap, &oi, &seen);
  sterling_field(pic, m2 + 1, m3, shill, out, cap, &oi, &seen);
  sterling_field(pic, m3 + 1, piclen, pence, out, cap, &oi, &seen);
  return oi;
}

/* Sterling P input (B1): read the picture-width field and recover its pence
 * value. The picture positions decide which characters are pounds, shillings
 * and pence digits; separators are consumed but carry no value. */
long long pli_get_edit_pic_sterling(long long w, const char *pic, long long piclen) {
  char field[128];
  long long i = 0;
  rt_in_lock();
  for (; i < w && i < (long long)(sizeof field - 1); ++i) {
    int c = rt_next_char();
    if (c == EOF)
      break;
    field[i] = (char)c;
  }
  rt_in_unlock();
  long long nread = i;
  while (i < (long long)(sizeof field - 1))
    field[i++] = ' ';
  field[i] = '\0';
  long long m1 = -1, m2 = -1, m3 = -1;
  for (i = 0; i < piclen; ++i)
    if (pic[i] == 'M') {
      if (m1 < 0)
        m1 = i;
      else if (m2 < 0)
        m2 = i;
      else if (m3 < 0)
        m3 = i;
    }
  if (m1 < 0 || m2 < 0 || m3 < 0)
    return 0;
  long long pos = 0;
  long long pounds = sterling_read_field(pic, m1 + 1, m2, field, &pos);
  long long shill = sterling_read_field(pic, m2 + 1, m3, field, &pos);
  long long pence = sterling_read_field(pic, m3 + 1, piclen, field, &pos);
  long long total = pounds * 240 + shill * 12 + pence;
  int neg = 0;
  for (i = 0; i < nread; ++i)
    if (field[i] == '-')
      neg = 1;
  return neg ? -total : total;
}

/* P picture-directed output: format and write the field. */
void pli_put_edit_pic(long long val, long long scale, int sign,
                      const char *pic, long long piclen) {
  char outbuf[128];
  rt_out_lock();
  long long n = pic_format128(pli_from_i64(val), scale, sign, pic, piclen, outbuf,
                              (long long)sizeof(outbuf) - 1);
  rt_put_raw(outbuf, n);
  rt_out_unlock();
}

/* Wide P output (B1): the value arrives as explicit (lo, hi) halves, the
 * same ABI rule as the other 128-bit DECIMAL entry points (ADR-191). */
void pli_put_edit_pic_wide(long long lo, long long hi, long long scale, int sign,
                           const char *pic, long long piclen) {
  char outbuf[128];
  PliI128 val = pli_to_s(pli_u128((uint64_t)lo, (uint64_t)hi));
  rt_out_lock();
  long long n = pic_format128(val, scale, sign, pic, piclen, outbuf,
                              (long long)sizeof(outbuf) - 1);
  rt_put_raw(outbuf, n);
  rt_out_unlock();
}

/* PICTURE variable assignment (ADR-290): the same formatter, into the
 * variable's CHARACTER storage. Returns the number of characters written. */
long long pli_pic_format(long long val, long long scale, int sign,
                         const char *pic, long long piclen,
                         char *dst, long long cap) {
  return pic_format128(pli_from_i64(val), scale, sign, pic, piclen, dst, cap);
}

long long pli_pic_format_wide(long long lo, long long hi, long long scale, int sign,
                              const char *pic, long long piclen,
                              char *dst, long long cap) {
  PliI128 val = pli_to_s(pli_u128((uint64_t)lo, (uint64_t)hi));
  return pic_format128(val, scale, sign, pic, piclen, dst, cap);
}

long long pli_pic_format_sterling(long long val, int sign, const char *pic, long long piclen,
                                  char *dst, long long cap) {
  return sterling_format(val, sign, pic, piclen, dst, cap);
}

void pli_put_edit_pic_sterling(long long val, int sign, const char *pic, long long piclen) {
  char outbuf[128];
  rt_out_lock();
  long long n = sterling_format(val, sign, pic, piclen, outbuf,
                                (long long)sizeof(outbuf) - 1);
  rt_put_raw(outbuf, n);
  rt_out_unlock();
}

