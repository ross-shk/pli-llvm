/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
 * C baseline for pure_call: the same loop, with the helper kept out of line
 * so the call is a real call — the PL/I version cannot mark a top-level
 * procedure for inlining either, because it is externally linked. Clang
 * infers the same memory effect for the helper on its own, which is what the
 * PL/I compiler has to work out for itself. */
#include <stdio.h>
#include <stdint.h>

__attribute__((noinline)) static int32_t helper(int32_t *x) {
    int32_t y = *x * 3 + 1;
    return y;
}

int main(void) {
    static int32_t a[1000];
    int32_t k = 3;
    long long s = 0;

    for (int32_t i = 1; i <= 1000; i++)
        a[i - 1] = i * 7 - 3;

    for (int32_t i = 1; i <= 150000000; i++) {
        int32_t j = i % 1000;
        int32_t x = a[j];
        s += x + helper(&k);
    }

    printf("%lld\n", (long long)s);
    return 0;
}
