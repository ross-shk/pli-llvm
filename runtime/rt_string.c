/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* rt_string.c — PL/I runtime library (libpli): string semantics + helpers. */
/* Split from pli_rt.c; pli_rt.h + pli_rt_abi.def stay the single ABI source. */
#include "pli_rt.h"
#include <stdio.h>
#include <time.h>

void pli_concat(char *dst, const char *a, long long alen, const char *b, long long blen) {
  if (alen > 0) pli_memmove(dst, a, (size_t)alen);
  if (blen > 0) pli_memmove(dst + alen, b, (size_t)blen);
}

/* DATE(): write the current local date as 'YYYYMMDD'. */
void pli_date(char *buf, long long cap) {
  time_t now = time(NULL);
  struct tm tmv;
#ifdef _WIN32
  localtime_s(&tmv, &now);
#else
  localtime_r(&now, &tmv);
#endif
  char tmp[16];
  int n = pli_snprintf(tmp, sizeof tmp, "%04d%02d%02d",
                   tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
  long long i = 0;
  for (; i < cap && i < n; ++i) buf[i] = tmp[i];
  for (; i < cap; ++i) buf[i] = ' ';
}

/* TIME(): write the current local time as 'HHMMSS'. */
void pli_time(char *buf, long long cap) {
  time_t now = time(NULL);
  struct tm tmv;
#ifdef _WIN32
  localtime_s(&tmv, &now);
#else
  localtime_r(&now, &tmv);
#endif
  char tmp[16];
  int n = pli_snprintf(tmp, sizeof tmp, "%02d%02d%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  long long i = 0;
  for (; i < cap && i < n; ++i) buf[i] = tmp[i];
  for (; i < cap; ++i) buf[i] = ' ';
}

/* RANK(c): code point of the first character, 0 when empty. */
long long pli_rank(const char *s, long long slen) {
  return slen > 0 ? (unsigned char)s[0] : 0;
}

/* COLLATE(n): the character with code n mod 256, blank-padding the tail. */
void pli_collate(char *dst, long long dstcap, long long n) {
  if (dstcap > 0)
    dst[0] = (char)(n & 0xFF);
  for (long long i = 1; i < dstcap; ++i)
    dst[i] = ' ';
}
