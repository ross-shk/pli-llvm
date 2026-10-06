/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* C baseline for division benchmark */
#include <stdio.h>

int main(void) {
    int i, j, denom, result;
    long long sum;

    sum = 0;
    for (i = 1; i <= 600000; i++) {
        for (j = 1; j <= 100; j++) {
            denom = (j % 17) + 1;
            result = i / denom;
            sum = sum + result;
        }
    }

    printf("%lld\n", sum);
    return 0;
}
