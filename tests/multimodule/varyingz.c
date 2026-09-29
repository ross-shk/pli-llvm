/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* VARYINGZ proof (ADR-168): the C side receives a NUL-terminated `char *`
   with no explicit length, so it can use ordinary string scans. */
#include <stddef.h>

int vzt_len(const char *s) {
  int n = 0;
  while (s[n] != '\0')
    n++;
  return n;
}

int vzt_at(const char *s, int i) { return (unsigned char)s[i]; }
