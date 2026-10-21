/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
 * C baseline for bit_merge: the same two masked field tests on one 32-bit
 * word, in one conjunction. */
#include <stdio.h>
#include <stdint.h>

int main(void) {
    int hits = 0;
    for (int i = 1; i <= 300000000; i++) {
        uint32_t w = (uint32_t)((i % 700) * 3000001);
        if ((w & 0x3000u) == 0x3000u && (w & 0x0300u) == 0x0300u) hits++;
    }
    printf("%d\n", hits);
    return 0;
}
