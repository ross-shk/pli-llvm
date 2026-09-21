/* rt_stream.c — PL/I runtime library (libpli): stream output: SKIP/PAGE, DISPLAY, PUT LIST/PUT DATA. */
/* Split from pli_rt.c; pli_rt.h + pli_rt_abi.def stay the single ABI source. */
#include "pli_rt.h"
#include <stdio.h>
#include <string.h>

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
      memmove(rt_out_buf + rt_out_len, p, take);
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
  int n = snprintf(buf, sizeof buf, "%lld", v);
  display_begin();
  rt_put_raw(buf, (size_t)n);
  display_end();
}
void pli_display_float(double v) {
  char buf[64];
  int n = snprintf(buf, sizeof buf, "%.6g", v);
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
  int n = snprintf(buf, sizeof buf, "%.6g%+.6gI", re, im);
  display_begin();
  rt_put_raw(buf, (size_t)n);
  display_end();
}

void pli_put_list_fixed(long long v) {
  char buf[32];
  int n = snprintf(buf, sizeof buf, "%lld", v);
  separate();
  rt_put_raw(buf, (size_t)n);
}

/* CHAR(fixed): decimal image of v, blank-padded to dstcap. */
void pli_char_of_fixed(char *dst, long long dstcap, long long v) {
  char tmp[24];
  int n = snprintf(tmp, sizeof tmp, "%lld", v);
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
  int n = snprintf(tmp, sizeof tmp, "%.6g", x);
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
  int n = snprintf(p, cap - (size_t)(p - buf), "%lld", v / factor);
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
  rt_put_raw(buf, strlen(buf));
}
void pli_display_decfixed(long long v, long long q) {
  char buf[64];
  format_decfixed(buf, sizeof buf, v, q);
  display_begin();
  rt_put_raw(buf, strlen(buf));
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
  int n = snprintf(p, sizeof buf - (size_t)(p - buf), "%lld", v / factor);
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
  int n = snprintf(buf, sizeof buf, "%.6g", v);
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
  int n = snprintf(buf, sizeof buf, "%.6g%+.6gI", re, im);
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

/* Assignment to CHARACTER(n) NONVARYING: truncate or pad with blanks. */

