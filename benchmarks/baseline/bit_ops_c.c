/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* C baseline for bit_ops benchmark */
#include <stdio.h>
#include <stdint.h>

int main(void) {
    long long i, sum, hash;
    uint32_t a, b, c, d, mask;

    a = 0xAAAAAAAAu;
    b = 0xFF00FF00u;
    mask = 0x0000FFFFu;

    sum = 0;
    hash = 1;
    for (i = 1; i <= 50000000; i++) {
        c = a & b;
        d = a | b;
        a = (c | (~d)) & mask;
        b = (a & c) | (~a);
        hash = (hash * 1103515245LL + 12345) & 0x7FFFFFFFLL;
        if (a != b) sum = sum + (hash % 100);
    }

    printf("%lld\n", sum);
    return 0;
}
