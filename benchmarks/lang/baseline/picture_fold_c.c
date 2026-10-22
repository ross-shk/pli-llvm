/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* C twin of the B4 picture-folding gate: the same 1000000 conversions of a
   constant value, formatted by snprintf and checksummed through the first
   byte, so the pair and the C ratio compare the same work. */
#include <stdio.h>

int main(void) {
  int i;
  int sum = 0;
  char b[21];
  for (i = 0; i < 1000000; i++) {
    snprintf(b, sizeof b, "%6.2f", 123.45);
    sum += (unsigned char)b[0];
  }
  printf("%d\n", sum);
  return 0;
}
