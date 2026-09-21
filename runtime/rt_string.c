/* rt_string.c — PL/I runtime library (libpli): string semantics + helpers. */
/* Split from pli_rt.c; pli_rt.h + pli_rt_abi.def stay the single ABI source. */
#include "pli_rt.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/* TRIM(s[, pad]): copy s minus leading/trailing blanks (or pad-set
 * characters), left-justified and blank-padded to dstcap. A null pad
 * means blanks; an empty pad set trims nothing. */
void pli_trim(char *dst, long long dstcap, const char *s, long long slen,
              const char *pad, long long padlen) {
  long long lo = 0, hi = slen;
  if (!pad) {
    while (lo < hi && s[lo] == ' ')
      ++lo;
    while (hi > lo && s[hi - 1] == ' ')
      --hi;
  } else {
    while (lo < hi && memchr(pad, s[lo], (size_t)padlen))
      ++lo;
    while (hi > lo && memchr(pad, s[hi - 1], (size_t)padlen))
      --hi;
  }
  long long out = 0;
  for (long long i = lo; i < hi && out < dstcap; ++i)
    dst[out++] = s[i];
  while (out < dstcap)
    dst[out++] = ' ';
}

/* TALLY(x, y): non-overlapping occurrences of y in x (Enterprise PL/I:
 * FIXED BINARY(31,0), case-sensitive, zero when absent or null). */
long long pli_tally(const char *x, long long xlen, const char *y, long long ylen) {
  if (ylen <= 0 || xlen < ylen)
    return 0;
  long long n = 0;
  for (long long i = 0; i + ylen <= xlen;) {
    if (memcmp(x + i, y, (size_t)ylen) == 0) {
      ++n;
      i += ylen;
    } else {
      ++i;
    }
  }
  return n;
}

/* UPPERCASE(s): copy s folding a-z to A-Z, blank-padding to dstcap. */
void pli_uppercase(char *dst, long long dstcap, const char *s, long long slen) {
  long long i = 0;
  for (; i < slen && i < dstcap; ++i) {
    char c = s[i];
    if (c >= 'a' && c <= 'z')
      c = (char)(c - 'a' + 'A');
    dst[i] = c;
  }
  while (i < dstcap)
    dst[i++] = ' ';
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

