/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
 * C baseline for checks_on: the same workload with no PL/I checks, so the
 * paired build difference is the cost of the SIZE and SUBSCRIPTRANGE
 * checks and nothing else. */
#include <stdio.h>
#include <stdint.h>

int main(void) {
    int32_t a[1000];
    int64_t s = 0, u = 0;

    for (int i = 0; i < 1000; i++)
        a[i] = (i + 1) * 7 - 3;    /* PL/I indexes from 1 */
    for (int i = 1; i <= 100000000; i++) {
        int j = i % 1000;
        int32_t x = a[j];
        int32_t y = x * 3 + 1;
        s += y;
        u += x;
    }
    printf("%lld\n%lld\n", (long long)s, (long long)u);
    return 0;
}
