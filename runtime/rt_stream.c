/* rt_stream.c — PL/I runtime library (libpli): stream output: SKIP/PAGE, DISPLAY, PUT LIST/PUT DATA. */
/* Split from pli_rt.c; pli_rt.h + pli_rt_abi.def stay the single ABI source. */
#include "pli_rt.h"
#include <stdarg.h>
#include <stdio.h>

/* SYSPRINT state. A full implementation tracks page/line/column against
 * LINESIZE and PAGESIZE and raises ENDPAGE; M0 tracks the column only. */
int rt_col = 0;
int rt_items_on_line = 0;
/* Data-directed output (rule (106), QR1.5): names emitted in the open DATA
 * list, and a flag suppressing the value's blank separator after NAME=. */
int rt_data_items = 0;
int rt_data_value_next = 0;
/* Set when get_token terminates at ';' (data-directed pairs, rule (106)),
 * so pli_get_data_next ends the list there. */
int rt_tok_semi = 0;

/* STRING (rule 105) sink/source: when rt_out_buf is non-null, list-directed output
 * is written into it instead of stdout; when rt_in_buf is non-null, list-directed
 * input is read from it instead of stdin. */
char *rt_out_buf = NULL;
size_t rt_out_cap = 0, rt_out_len = 0;
const char *rt_in_buf = NULL;
size_t rt_in_len = 0, rt_in_pos = 0;

/* Named files (rules 100-103, 105): a fixed table indexed by the compile-time
 * slot assigned to each FILE variable. rt_out_f/rt_in_f are the streams selected by
 * the FILE ( f ) option of a PUT/GET; when set they override stdout/stdin. */
#define PLI_MAX_FILES 16
FILE *rt_pli_files[PLI_MAX_FILES] = {0};
FILE *rt_out_f = NULL;
FILE *rt_in_f = NULL;

void rt_put_raw(const char *p, size_t n) {
  if (rt_out_f) {
    fwrite(p, 1, n, rt_out_f);
    return;
  }
  if (rt_out_buf) {
    size_t take = n < (rt_out_cap - rt_out_len) ? n : (rt_out_cap - rt_out_len);
    if (take > 0)
      pli_memmove(rt_out_buf + rt_out_len, p, take);
    rt_out_len += take;
    return;
  }
  fwrite(p, 1, n, stdout);
  rt_col += (int)n;
}

/* Read one input character from the active FILE stream, STRING source, or
 * stdin. */
int rt_next_char(void) {
  if (rt_in_f)
    return getc(rt_in_f);
  if (rt_in_buf)
    return rt_in_pos < rt_in_len ? (unsigned char)rt_in_buf[rt_in_pos++] : EOF;
  return getchar();
}

void pli_put_skip(long long n) {
  if (n < 1) n = 1;
  if (rt_col > 0) {
    fputc('\n', stdout);
    rt_col = 0;
  }
  for (long long i = 1; i < n; ++i) fputc('\n', stdout);
  rt_items_on_line = 0;
}

void pli_put_page(void) {
  if (rt_col > 0) fputc('\n', stdout);
  fputc('\f', stdout);
  rt_col = 0;
  rt_items_on_line = 0;
}

/* List-directed output separates successive items by a blank; character
 * values are written without enclosing quotation marks. After a DATA name
 * the value follows its '=' directly, with no blank. */
void rt_separate(void) {
  if (rt_data_value_next) {
    rt_data_value_next = 0;
    rt_items_on_line++;
    return;
  }
  if (rt_items_on_line > 0) rt_put_raw(" ", 1);
  rt_items_on_line++;
}

void pli_put_list_char(const char *p, long long len) {
  separate();
  if (len > 0) rt_put_raw(p, (size_t)len);
}

/* DISPLAY (rule 114, ADR-081): one scalar value plus a newline, with the
 * same value formats as list-directed output but no item separator. */
void rt_display_end(void) {
  // The line is complete: reset the column and item count so fini and a
  // following PUT start clean.
  rt_put_raw("\n", 1);
  rt_col = 0;
  rt_items_on_line = 0;
}
/* A DISPLAY starts on a fresh line: end a pending PUT line first (as SKIP
 * does), so mixed PUT/DISPLAY output never joins two values on one line. */
void rt_display_begin(void) {
  if (rt_col > 0) {
    rt_put_raw("\n", 1);
    rt_col = 0;
  }
  rt_items_on_line = 0;
}
void pli_display_char(const char *p, long long len) {
  display_begin();
  if (len > 0) rt_put_raw(p, (size_t)len);
  display_end();
}
void pli_display_fixed(long long v) {
  char buf[32];
  int n = pli_snprintf(buf, sizeof buf, "%lld", v);
  display_begin();
  rt_put_raw(buf, (size_t)n);
  display_end();
}
void pli_display_float(double v) {
  char buf[64];
  int n = pli_snprintf(buf, sizeof buf, "%.6g", v);
  display_begin();
  rt_put_raw(buf, (size_t)n);
  display_end();
}
void pli_display_bit(unsigned char b) {
  display_begin();
  rt_put_raw(b ? "1" : "0", 1);
  display_end();
}
void pli_display_complex(double re, double im) {
  char buf[128];
  int n = pli_snprintf(buf, sizeof buf, "%.6g%+.6gI", re, im);
  display_begin();
  rt_put_raw(buf, (size_t)n);
  display_end();
}

void pli_put_list_fixed(long long v) {
  char buf[32];
  int n = pli_snprintf(buf, sizeof buf, "%lld", v);
  separate();
  rt_put_raw(buf, (size_t)n);
}

/* CHAR(fixed): decimal image of v, blank-padded to dstcap. */
void pli_char_of_fixed(char *dst, long long dstcap, long long v) {
  char tmp[24];
  int n = pli_snprintf(tmp, sizeof tmp, "%lld", v);
  long long i = 0;
  for (; i < n && i < dstcap; ++i)
    dst[i] = tmp[i];
  for (; i < dstcap; ++i)
    dst[i] = ' ';
}

/* CHAR(float): %.6g image (the same readable choice as PUT LIST output),
 * blank-padded to dstcap. */
void pli_char_of_float(char *dst, long long dstcap, double x) {
  char tmp[48];
  int n = pli_snprintf(tmp, sizeof tmp, "%.6g", x);
  long long i = 0;
  while (i < n && i < dstcap) {
    dst[i] = tmp[i];
    ++i;
  }
  for (; i < dstcap; ++i)
    dst[i] = ' ';
}

/* FIXED(char): parse decimal text, truncating any fraction toward zero.
 * Leading blanks, one optional sign, then digits; stops at the first
 * non-digit (no CONVERSION condition in this stage). */
long long pli_fixed_of_char(const char *s, long long slen) {
  long long i = 0;
  while (i < slen && (s[i] == ' ' || s[i] == '\t'))
    ++i;
  int neg = 0;
  if (i < slen && (s[i] == '+' || s[i] == '-')) {
    neg = s[i] == '-';
    ++i;
  }
  long long v = 0;
  while (i < slen && s[i] >= '0' && s[i] <= '9') {
    v = v * 10 + (s[i] - '0');
    ++i;
  }
  return neg ? -v : v;
}

/* FIXED(float): truncate toward zero (NaN reads as 0, out-of-range clamps;
 * SIZE routing for the clamping case is a follow-up). */
long long pli_fixed_of_float(double x) {
  if (x != x)
    return 0;
  if (x >= 9223372036854775807.0)
    return 9223372036854775807LL;
  if (x < -9223372036854775808.0)
    return (-9223372036854775807LL - 1);
  return (long long)x;
}

/* FIXED DECIMAL output (QR1.2): the stored integer holds value * 10^q, so
 * print exactly q fraction digits. */
void rt_format_decfixed(char *buf, size_t cap, long long v, long long q) {
  char *p = buf;
  if (v < 0) {
    *p++ = '-';
    v = -v;
  }
  long long factor = 1;
  for (long long i = 0; i < q && i < 18; ++i)
    factor *= 10;
  int n = pli_snprintf(p, cap - (size_t)(p - buf), "%lld", v / factor);
  p += n;
  if (q > 0) {
    *p++ = '.';
    long long rem = v % factor;
    for (long long i = q - 1; i >= 0; --i) {
      long long digit = 1;
      for (long long j = 0; j < i; ++j)
        digit *= 10;
      *p++ = (char)('0' + rem / digit);
      rem %= digit;
    }
  }
  *p = '\0';
}
void pli_put_list_decfixed(long long v, long long q) {
  char buf[64];
  format_decfixed(buf, sizeof buf, v, q);
  separate();
  rt_put_raw(buf, pli_strlen(buf));
}
void pli_display_decfixed(long long v, long long q) {
  char buf[64];
  format_decfixed(buf, sizeof buf, v, q);
  display_begin();
  rt_put_raw(buf, pli_strlen(buf));
  display_end();
}

/* List-directed output of a scaled FIXED value (ADR-006): the stored integer v
 * holds the value * 2^scale, so print v / 2^scale exactly as a decimal. The
 * fractional part of a dyadic rational terminates, so the digit loop is exact. */
void pli_put_list_fixed_scaled(long long v, long long scale) {
  char buf[96];
  char *p = buf;
  if (scale <= 0) {  /* fall back to the plain formatter */
    pli_put_list_fixed(v);
    return;
  }
  if (v < 0) { *p++ = '-'; v = -v; }
  long long factor = 1LL << scale;  /* 2^scale */
  int n = pli_snprintf(p, sizeof buf - (size_t)(p - buf), "%lld", v / factor);
  p += n;
  long long rem = v % factor;
  if (rem != 0) {
    *p++ = '.';
    char digit[64];
    int cnt = 0;
    while (rem != 0 && cnt < 60) {
      rem *= 10;
      digit[cnt++] = (char)(rem >> scale);
      rem &= (factor - 1);
    }
    for (int i = 0; i < cnt; ++i) *p++ = '0' + digit[i];
  }
  separate();
  rt_put_raw(buf, (size_t)(p - buf));
}

void pli_put_list_float(double v) {
  char buf[64];
  /* PL/I would use E-format for FLOAT; %.6g keeps the wireframe readable. */
  int n = pli_snprintf(buf, sizeof buf, "%.6g", v);
  separate();
  rt_put_raw(buf, (size_t)n);
}

void pli_put_list_bit(unsigned char b) {
  separate();
  rt_put_raw(b ? "1" : "0", 1);
}

/* Complex output (CM5, ADR-085): real part, sign, imaginary magnitude, I. */
void pli_put_list_complex(double re, double im) {
  char buf[128];
  int n = pli_snprintf(buf, sizeof buf, "%.6g%+.6gI", re, im);
  separate();
  rt_put_raw(buf, (size_t)n);
}

/* Data-directed output (rule (106), QR1.5): `NAME=` opens an item (", "
 * between items) and the value follows with no blank; `;` closes the list. */
void pli_put_data_name(const char *p, long long len) {
  if (rt_data_items > 0) rt_put_raw(", ", 2);
  if (len > 0) rt_put_raw(p, (size_t)len);
  rt_put_raw("=", 1);
  rt_data_items++;
  rt_data_value_next = 1;
}

void pli_put_data_end(void) {
  rt_put_raw(";", 1);
  rt_items_on_line++;
  rt_data_items = 0;
  rt_data_value_next = 0;
}

/* ---------------------------------------------------------------------------
 * Ported minimal formatter (runtime-reduction plan, Phase 3).
 *
 * A small snprintf supporting exactly the conversions the runtime emits:
 *   %d %lld            signed decimal (int / long long)
 *   %0*lld %04d %02d   zero-padded to a fixed width (numeric or '*')
 *   %f %.6f %.*f       fixed-point, optional precision
 *   %e %.*e            scientific (d.ddde±XX)
 *   %g %.6g %+.6g      general: %f/%e chosen by magnitude, trailing zeros
 *                      stripped; '+' flag for the complex-image sign
 *   %s %c              (unused, provided for completeness)
 * No %p/%n/%a/%u/%zu, no locale, no left/right space padding beyond '0'.
 * Returns the would-be length (snprintf semantics); writes at most cap-1
 * characters plus the NUL terminator.
 * ------------------------------------------------------------------------- */

typedef struct {
  char *buf;
  size_t cap;
  size_t len;
} Fmt;

static void fputc_(Fmt *f, char c) {
  if (f->len + 1 < f->cap)
    f->buf[f->len] = c;
  f->len++;
}

static void fputs_(Fmt *f, const char *s, size_t n) {
  for (size_t i = 0; i < n; ++i)
    fputc_(f, s[i]);
}

static void fpad_(Fmt *f, char c, size_t n) {
  while (n--)
    fputc_(f, c);
}

static void fmt_int(Fmt *f, long long v, int width, int zero) {
  char tmp[24];
  int n = 0;
  unsigned long long u = v < 0 ? (unsigned long long)(-(v + 1)) + 1
                               : (unsigned long long)v;
  do {
    tmp[n++] = (char)('0' + u % 10);
    u /= 10;
  } while (u);
  int sign = v < 0;
  int pad = width - (n + sign);
  if (pad < 0)
    pad = 0;
  if (!zero)
    fpad_(f, ' ', (size_t)pad);
  if (sign)
    fputc_(f, '-');
  if (zero)
    fpad_(f, '0', (size_t)pad);
  while (n)
    fputc_(f, tmp[--n]);
}

static double pow10_i(int e) {
  double r = 1.0;
  if (e >= 0)
    for (int i = 0; i < e; ++i)
      r *= 10.0;
  else
    for (int i = 0; i < -e; ++i)
      r /= 10.0;
  return r;
}

/* Decimal exponent E of v >= 0 such that 1 <= v*10^-E < 10 (E = floor(log10 v),
 * 0 for v == 0). This is the %g/%e exponent. */
static int dec_exp(double v) {
  if (v == 0.0)
    return 0;
  int e = 0;
  if (v >= 1.0) {
    while (v >= 10.0) {
      v /= 10.0;
      ++e;
    }
  } else {
    while (v < 1.0) {
      v *= 10.0;
      --e;
    }
  }
  return e;
}

/* True when v is negative (including -0.0), without needing signbit(). */
static int is_neg(double v) {
  return v < 0.0 || (v == 0.0 && 1.0 / v < 0.0);
}

/* Print a non-negative v with p digits after the decimal point (no sign),
 * rounding half to even (printf semantics). The whole part is printed
 * directly (no overflow for large magnitudes); only the fraction in [0,1) is
 * scaled by 10^p, exact for p <= 18 (clamped). The half-way tie is broken to
 * even, where the retained last digit's parity must account for the whole
 * part when p == 0 (rounding to an integer). Avoids libm floor/ceil. */
static void fmt_fixed(Fmt *f, double v, int p) {
  if (p < 0)
    p = 0;
  if (p > 18)
    p = 18;
  long long whole = (long long)v;
  double frac0 = v - (double)whole;
  long long s = 1;
  for (int i = 0; i < p; ++i)
    s *= 10;
  long long ri = 0;
  if (p == 0) {
    if (frac0 > 0.5 || (frac0 == 0.5 && (whole & 1)))
      ++whole;
  } else {
    double scaled = frac0 * (double)s;
    long long ftr = (long long)scaled;
    double ffr = scaled - (double)ftr;
    if (ffr > 0.5)
      ri = ftr + 1;
    else if (ffr < 0.5)
      ri = ftr;
    else
      ri = (ftr & 1) ? ftr + 1 : ftr; /* exactly .5: round to even */
    if (ri >= s) {
      ++whole;
      ri = 0;
    }
  }
  char tmp[32];
  int n = 0;
  unsigned long long u = (unsigned long long)whole;
  do {
    tmp[n++] = (char)('0' + u % 10);
    u /= 10;
  } while (u);
  while (n)
    fputc_(f, tmp[--n]);
  if (p > 0) {
    fputc_(f, '.');
    char ft[64];
    int fn = 0;
    unsigned long long fu = (unsigned long long)ri;
    do {
      ft[fn++] = (char)('0' + fu % 10);
      fu /= 10;
    } while (fu);
    while (fn < p)
      ft[fn++] = '0';
    while (fn)
      fputc_(f, ft[--fn]);
  }
}

/* Emit 'e<sign><exponent>' with at least two exponent digits. */
static void fmt_exp(Fmt *f, int exp) {
  fputc_(f, 'e');
  int ne = exp < 0 ? -exp : exp;
  fputc_(f, exp < 0 ? '-' : '+');
  char ed[16];
  int n2 = 0;
  do {
    ed[n2++] = (char)('0' + ne % 10);
    ne /= 10;
  } while (ne);
  while (n2 < 2)
    ed[n2++] = '0';
  while (n2)
    fputc_(f, ed[--n2]);
}

/* True when the mantissa text in f rolled over a power of ten: it reads
 * "10" or "10.xxx" (a mantissa in [1,10) that rounded up to 10). */
static int rolled_over(const Fmt *f) {
  size_t i = 0;
  while (i < f->len && f->buf[i] != '.')
    ++i;
  return i == 2 && f->buf[0] == '1' && f->buf[1] == '0';
}

/* %e helper: sign handled by caller; v >= 0. */
static void fmt_sci(Fmt *f, double v, int p) {
  if (p < 0)
    p = 0;
  if (p > 60)
    p = 60;
  int exp = dec_exp(v);
  double m = v / pow10_i(exp);
  char tmp[512];
  Fmt tf;
  tf.buf = tmp;
  tf.cap = sizeof tmp;
  tf.len = 0;
  fmt_fixed(&tf, m, p);
  if (rolled_over(&tf)) {
    ++exp;
    tf.len = 0;
    fmt_fixed(&tf, 1.0, p);
  }
  fputs_(f, tmp, tf.len);
  fmt_exp(f, exp);
}

/* %g trailing-zero strip: drop trailing zeros after the '.', then the '.'. */
static void strip_g(Fmt *f) {
  char *s = f->buf;
  char *dot = NULL;
  for (size_t i = 0; i < f->len; ++i)
    if (s[i] == '.') {
      dot = &s[i];
      break;
    }
  if (!dot)
    return;
  while (f->len > 0 && s[f->len - 1] == '0')
    --f->len;
  if (f->len > 0 && s[f->len - 1] == '.')
    --f->len;
}

/* %g helper: sign handled by caller; v >= 0; P significant digits. */
static void fmt_gen(Fmt *f, double v, int P) {
  if (P < 0)
    P = 0;
  if (P > 60)
    P = 60;
  char tmp[512];
  Fmt tf;
  tf.buf = tmp;
  tf.cap = sizeof tmp;
  tf.len = 0;
  int x = dec_exp(v);
  if (x < -4 || x >= P) {
    int exp = x;
    double m = v / pow10_i(exp);
    fmt_fixed(&tf, m, P - 1);
    if (rolled_over(&tf)) {
      ++exp;
      tf.len = 0;
      fmt_fixed(&tf, 1.0, P - 1);
    }
    strip_g(&tf);
    fputs_(f, tmp, tf.len);
    fmt_exp(f, exp);
  } else {
    int p = P - 1 - x;
    fmt_fixed(&tf, v, p);
    /* If rounding carries the whole part to the next power of ten (e.g.
     * 999.9 -> 1000) and that new exponent E+1 >= P, %g renormalises to
     * scientific notation. Only relevant for values >= 1. */
    size_t wd = 0;
    while (wd < tf.len && tmp[wd] != '.')
      ++wd;
    if (x >= 0 && (int)wd == x + 2 && x + 1 >= P) {
      tf.len = 0;
      fmt_fixed(&tf, 1.0, P - 1);
      strip_g(&tf);
      fputs_(f, tmp, tf.len);
      fmt_exp(f, x + 1);
    } else {
      strip_g(&tf);
      fputs_(f, tmp, tf.len);
    }
  }
}

static void fmt_fixed_signed(Fmt *f, double v, int p, int plus) {
  if (v != v) {
    fputs_(f, "nan", 3);
    return;
  }
  if (is_neg(v)) {
    fputc_(f, '-');
    v = -v;
  } else if (plus) {
    fputc_(f, '+');
  }
  fmt_fixed(f, v, p);
}

static void fmt_sci_signed(Fmt *f, double v, int p, int plus) {
  if (v != v) {
    fputs_(f, "nan", 3);
    return;
  }
  if (is_neg(v)) {
    fputc_(f, '-');
    v = -v;
  } else if (plus) {
    fputc_(f, '+');
  }
  fmt_sci(f, v, p);
}

static void fmt_gen_signed(Fmt *f, double v, int P, int plus) {
  if (v != v) {
    fputs_(f, "nan", 3);
    return;
  }
  if (is_neg(v)) {
    fputc_(f, '-');
    v = -v;
  } else if (plus) {
    fputc_(f, '+');
  }
  fmt_gen(f, v, P);
}

int pli_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
  Fmt f;
  f.buf = buf;
  f.cap = cap;
  f.len = 0;
  const char *p = fmt;
  while (*p) {
    if (*p != '%') {
      fputc_(&f, *p++);
      continue;
    }
    ++p;
    if (*p == '%') {
      fputc_(&f, '%');
      ++p;
      continue;
    }
    int zero = 0, plus = 0;
    for (;;) {
      if (*p == '0') {
        zero = 1;
        ++p;
      } else if (*p == '+') {
        plus = 1;
        ++p;
      } else {
        break;
      }
    }
    int width = 0;
    if (*p == '*') {
      width = va_arg(ap, int);
      ++p;
    } else {
      while (*p >= '0' && *p <= '9')
        width = width * 10 + (*p++ - '0');
    }
    int prec = -1;
    if (*p == '.') {
      ++p;
      if (*p == '*') {
        prec = va_arg(ap, int);
        ++p;
      } else {
        prec = 0;
        while (*p >= '0' && *p <= '9')
          prec = prec * 10 + (*p++ - '0');
      }
    }
    char conv = *p++;
    int llong = 0;
    if (conv == 'l' && *p == 'l') {
      llong = 1;
      ++p;
      conv = *p++;
    }
    switch (conv) {
    case 'd':
    case 'i': {
      long long iv = llong ? va_arg(ap, long long) : va_arg(ap, int);
      fmt_int(&f, iv, width, zero);
      break;
    }
    case 'f': {
      double dv = va_arg(ap, double);
      if (prec < 0)
        prec = 6;
      fmt_fixed_signed(&f, dv, prec, plus);
      break;
    }
    case 'e': {
      double dv = va_arg(ap, double);
      if (prec < 0)
        prec = 6;
      fmt_sci_signed(&f, dv, prec, plus);
      break;
    }
    case 'g': {
      double dv = va_arg(ap, double);
      if (prec < 0)
        prec = 6;
      fmt_gen_signed(&f, dv, prec, plus);
      break;
    }
    case 's': {
      const char *s2 = va_arg(ap, const char *);
      fputs_(&f, s2, pli_strlen(s2));
      break;
    }
    case 'c':
      fputc_(&f, (char)va_arg(ap, int));
      break;
    default:
      fputc_(&f, conv);
      break;
    }
  }
  if (cap > 0) {
    if (f.len < cap)
      buf[f.len] = '\0';
    else
      buf[cap - 1] = '\0';
  }
  return (int)f.len;
}

int pli_snprintf(char *buf, size_t cap, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = pli_vsnprintf(buf, cap, fmt, ap);
  va_end(ap);
  return n;
}

/* Assignment to CHARACTER(n) NONVARYING: truncate or pad with blanks. */

