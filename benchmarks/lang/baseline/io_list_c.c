/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
 * C baseline for io_list: the same list-directed line, one statement per
 * row, with the character item blank-padded to its declared width. */
#include <stdio.h>

int main(void) {
    long long s = 0;
    long long hits = 0;
    for (int i = 1; i <= 500000; i++) {
        printf("%d %d %d %d %-8s\n", i, i + 1, i + 2, i + 3, "item");
        s += i;
        hits++;
    }
    printf("%lld\n%lld\n", s, hits);
    return 0;
}
