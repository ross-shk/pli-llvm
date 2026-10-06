/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* C baseline for collatz benchmark */
#include <stdio.h>

int main(void) {
    long i, m, n, steps, total;

    n = 400000;
    total = 0;
    for (i = 2; i <= n; i++) {
        steps = 0;
        m = i;
        while (m > 1) {
            if (m % 2 == 0)
                m = m / 2;
            else
                m = 3 * m + 1;
            steps = steps + 1;
        }
        total = total + steps;
    }

    printf("%ld\n", total);
    return 0;
}
