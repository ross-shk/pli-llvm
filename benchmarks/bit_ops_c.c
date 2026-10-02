/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* C baseline for bit_ops benchmark */
#include <stdio.h>
#include <stdint.h>

int main(void) {
    int i, sum;
    uint32_t a, b, c, d, mask;

    a = 0xAAAAAAAAu;
    b = 0xFF00FF00u;
    mask = 0x0000FFFFu;

    sum = 0;
    for (i = 0; i < 100000; i++) {
        c = a & b;
        d = a | b;
        a = (c | (~d)) & mask;
        b = (a & c) | (~a);
        if (a != b) sum = sum + 1;
    }

    printf("%d\n", sum);
    return 0;
}
