/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
 * C baseline for bit_fields: the same four field tests on one 32-bit word,
 * masked. */
#include <stdio.h>
#include <stdint.h>

int main(void) {
    const uint32_t m1 = 0xFu, m2 = 0xF0u, m3 = 0xF00u, m4 = 0xF000u;
    const uint32_t e1 = 0x3u, e2 = 0x30u, e3 = 0x200u, e4 = 0xB000u;
    int hits = 0;
    for (int i = 1; i <= 150000000; i++) {
        uint32_t w = (uint32_t)((i % 700) * 3000001);
        if ((w & m1) == e1) hits++;
        if ((w & m2) == e2) hits++;
        if ((w & m3) == e3) hits++;
        if ((w & m4) == e4) hits++;
    }
    printf("%d\n", hits);
    return 0;
}
