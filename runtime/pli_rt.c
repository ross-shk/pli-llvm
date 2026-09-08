/* pli_rt.c — PL/I runtime library (libpli), M0 subset. */
#include "pli_rt.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* SYSPRINT state. A full implementation tracks page/line/column against
 * LINESIZE and PAGESIZE and raises ENDPAGE; M0 tracks the column only. */
static int col = 0;
static int items_on_line = 0;

static void put_raw(const char *p, size_t n) {
  fwrite(p, 1, n, stdout);
  col += (int)n;
}

void pli_rt_init(void) {
  col = 0;
  items_on_line = 0;
}

void pli_rt_fini(void) {
  if (col > 0) {
    fputc('\n', stdout);
    col = 0;
  }
  fflush(stdout);
}

void pli_stop(void) {
  pli_rt_fini();
  exit(0);
}

/* SKIP(n): advance to the start of a line, then n-1 further lines.
 * M0 deviation: on a fresh line SKIP does not emit a blank line, so the
 * idiomatic `PUT SKIP LIST(...)` prints on the current line. */
void pli_put_skip(long long n) {
  if (n < 1) n = 1;
  if (col > 0) {
    fputc('\n', stdout);
    col = 0;
  }
  for (long long i = 1; i < n; ++i) fputc('\n', stdout);
  items_on_line = 0;
}

void pli_put_page(void) {
  if (col > 0) fputc('\n', stdout);
  fputc('\f', stdout);
  col = 0;
  items_on_line = 0;
}

/* List-directed output separates successive items by a blank; character
 * values are written without enclosing quotation marks. */
static void separate(void) {
  if (items_on_line > 0) put_raw(" ", 1);
  items_on_line++;
}

void pli_put_list_char(const char *p, long long len) {
  separate();
  if (len > 0) put_raw(p, (size_t)len);
}

void pli_put_list_fixed(long long v) {
  char buf[32];
  int n = snprintf(buf, sizeof buf, "%lld", v);
  separate();
  put_raw(buf, (size_t)n);
}

void pli_put_list_float(double v) {
  char buf[64];
  /* PL/I would use E-format for FLOAT; %.6g keeps the wireframe readable. */
  int n = snprintf(buf, sizeof buf, "%.6g", v);
  separate();
  put_raw(buf, (size_t)n);
}

void pli_put_list_bit(unsigned char b) {
  separate();
  put_raw(b ? "1" : "0", 1);
}

/* Assignment to CHARACTER(n) NONVARYING: truncate or pad with blanks. */
void pli_assign_char(char *dst, long long dstlen, const char *src, long long srclen) {
  long long n = srclen < dstlen ? srclen : dstlen;
  if (n > 0) memmove(dst, src, (size_t)n);
  if (dstlen > n) memset(dst + n, ' ', (size_t)(dstlen - n));
}

/* Assignment to CHARACTER(n) VARYING: truncate to the maximum length; the
 * caller stores the returned current length. */
long long pli_assign_varying(char *dstdata, long long cap, const char *src, long long srclen) {
  long long n = srclen < cap ? srclen : cap;
  if (n > 0) memmove(dstdata, src, (size_t)n);
  return n;
}

void pli_concat(char *dst, const char *a, long long alen, const char *b, long long blen) {
  if (alen > 0) memmove(dst, a, (size_t)alen);
  if (blen > 0) memmove(dst + alen, b, (size_t)blen);
}

/* SUBSTR(s, i, n): copy up to n characters of s starting at the 1-based
 * position i, blank-filling the tail. Positions past the end of s clip to
 * blanks (a real compiler would raise SUBSCRIPTRANGE, M3). */
void pli_substr(char *dst, long long dstcap, const char *src, long long srclen,
                long long start, long long len) {
  long long n = len < dstcap ? len : dstcap;
  long long avail = srclen - (start - 1);
  if (avail < 0) avail = 0;
  long long take = avail < n ? avail : n;
  if (take > 0 && start >= 1) memmove(dst, src + (start - 1), (size_t)take);
  if (n > take) memset(dst + take, ' ', (size_t)(n - take));
}

/* SUBSTR(s, i, n) = v: overwrite n characters of dst starting at the 1-based
 * position i with the first n characters of src, blank-filling the tail when
 * src is shorter than n. Positions past the end of dst clip (SUBSCRIPTRANGE
 * is M3). */
void pli_substr_assign(char *dst, long long dstcap, long long start, long long len,
                       const char *src, long long srclen) {
  if (start < 1) return;
  long long n = len;
  long long space = dstcap - (start - 1);
  if (space <= 0) return;
  if (n > space) n = space;
  long long take = srclen < n ? srclen : n;
  if (take > 0) memmove(dst + (start - 1), src, (size_t)take);
  if (n > take) memset(dst + (start - 1) + take, ' ', (size_t)(n - take));
}

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

/* REPEAT(s, n): fill dst with n copies of s, blank-padding to dstcap. */
void pli_repeat(char *dst, long long dstcap, const char *src, long long srclen,
                long long n) {
  long long out = 0;
  for (long long k = 0; k < n; ++k)
    for (long long i = 0; i < srclen && out < dstcap; ++i) dst[out++] = src[i];
  while (out < dstcap) dst[out++] = ' ';
}

/* VERIFY(s, t): 1-based position of the first character of s not in t, or 0
 * if every character of s appears in t (Y33-6003 VERIFY). */
long long pli_verify(const char *s, long long slen, const char *t, long long tlen) {
  for (long long i = 0; i < slen; ++i) {
    long long j = 0;
    while (j < tlen && s[i] != t[j]) ++j;
    if (j == tlen) return i + 1;
  }
  return 0;
}

/* TRANSLATE(s, out, in): copy s, mapping each character that occurs in `in`
 * to the corresponding character of `out`; unmatched characters pass through. */
void pli_translate(char *dst, long long dstcap, const char *s, long long slen,
                   const char *out, long long outlen, const char *in, long long inlen) {
  for (long long i = 0; i < slen && i < dstcap; ++i) {
    char c = s[i];
    long long k = 0;
    while (k < inlen && c != in[k]) ++k;
    if (k < inlen && k < outlen) c = out[k];
    dst[i] = c;
  }
}

/* HIGH(n): n copies of the highest collating character (0xFF). */
void pli_high(char *dst, long long n) { memset(dst, 0xFF, (size_t)n); }

/* LOW(n): n copies of the lowest collating character (0x00). */
void pli_low(char *dst, long long n) { memset(dst, 0x00, (size_t)n); }

/* DATE(): write the current local date as 'YYYYMMDD'. */
void pli_date(char *buf, long long cap) {
  time_t now = time(NULL);
  struct tm tmv;
  localtime_r(&now, &tmv);
  char tmp[16];
  int n = snprintf(tmp, sizeof tmp, "%04d%02d%02d",
                   tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
  long long i = 0;
  for (; i < cap && i < n; ++i) buf[i] = tmp[i];
  for (; i < cap; ++i) buf[i] = ' ';
}

/* TIME(): write the current local time as 'HHMMSS'. */
void pli_time(char *buf, long long cap) {
  time_t now = time(NULL);
  struct tm tmv;
  localtime_r(&now, &tmv);
  char tmp[16];
  int n = snprintf(tmp, sizeof tmp, "%02d%02d%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  long long i = 0;
  for (; i < cap && i < n; ++i) buf[i] = tmp[i];
  for (; i < cap; ++i) buf[i] = ' ';
}

/* Comparison of character data: the shorter operand is notionally extended
 * with blanks on the right. */
int pli_cmp_char(const char *a, long long alen, const char *b, long long blen) {
  long long n = alen > blen ? alen : blen;
  for (long long i = 0; i < n; ++i) {
    unsigned char ca = i < alen ? (unsigned char)a[i] : (unsigned char)' ';
    unsigned char cb = i < blen ? (unsigned char)b[i] : (unsigned char)' ';
    if (ca != cb) return ca < cb ? -1 : 1;
  }
  return 0;
}

/* INDEX(s1, s2): 1-based position of the first occurrence of s2 within s1, or
 * 0 if not present. An empty s2 matches at position 1 (Y33-6003 INDEX). */
long long pli_index(const char *a, long long alen, const char *b, long long blen) {
  if (blen <= 0) return 1;
  if (blen > alen) return 0;
  for (long long i = 0; i + blen <= alen; ++i) {
    long long j = 0;
    while (j < blen && a[i + j] == b[j]) ++j;
    if (j == blen) return i + 1;
  }
  return 0;
}
void pli_signal_error(const char *msg) {
  pli_rt_fini();
  fprintf(stderr, "ERROR condition raised: %s\n", msg ? msg : "(unspecified)");
  exit(8);
}
