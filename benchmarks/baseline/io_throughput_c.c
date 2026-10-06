/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* C baseline for io_throughput benchmark */
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    int i;
    long long total, sq;
    total = 0;
    for (i = 1; i <= 200000; i++) {
        sq = (long long)i * i;  /* 64-bit: i*i overflows 32-bit past i=46341 */
        printf("%d %lld %d\n", i, sq, abs(5000 - i * 3));
        total += i;
    }
    printf("%lld\n", total);
    return 0;
}
