/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* C baseline for fixed_bin63 benchmark */
#include <stdio.h>
#include <stdint.h>

int main(void) {
    long long i, n, acc, sum, hash;
    int a31, b31, w31;

    acc = 0;
    sum = 0;
    hash = 1;
    n = 30000000;

    for (i = 1; i <= n; i++) {
        a31 = hash % 7919;
        b31 = hash % 31;
        w31 = a31 + b31;             /* 31-bit arithmetic */
        acc = acc + w31 + hash;      /* w31 widens to 64-bit */
        hash = (hash * 1103515245LL + 12345) % 2147483648LL;
        sum = sum + (acc % 997);
    }

    printf("%lld\n", sum);
    return 0;
}
