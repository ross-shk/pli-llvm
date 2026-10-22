/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
 * C baseline for on_dispatch: the same loop with the trap replaced by an
 * explicit test, so the ratio is the cost of the ON-unit machinery. */
#include <stdio.h>
#include <stdint.h>

int main(void) {
    int64_t s = 0, hits = 0;
    for (int i = 1; i <= 60000000; i++) {
        int d = i % 100;
        int32_t q = 0;
        if (d != 0)
            q = 1000000 / d;
        else {
            hits++;
            s++;          /* the ON-unit adds to the accumulator too */
        }
        s += q;
    }
    printf("%lld\n%lld\n", (long long)s, (long long)hits);
    return 0;
}
