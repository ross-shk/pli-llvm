/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
 * C baseline for concat_const: the two halves are copied into the result
 * at constant offsets. */
#include <stdio.h>
#include <string.h>

int main(void) {
    static const char srcs[4][21] = {
        "abcdefghij0123456789",
        "ABCDEFGHIJ0123456789",
        "zyxwvutsrq9876543210",
        "0123456789abcdefghij"
    };
    char a[11], b[11], c[21];
    int hits = 0;
    for (int i = 1; i <= 4000000; i++) {
        int j = (i % 4) + 1;
        memcpy(a, srcs[j], 10);
        memcpy(b, srcs[j] + 10, 10);
        memcpy(c, a, 10);
        memcpy(c + 10, b, 10);
        if (c[0] == 'A')
            hits++;
    }
    printf("%d\n", hits);
    return 0;
}
