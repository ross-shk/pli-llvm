/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* C baseline for controlled_stack benchmark */
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    long long s = 0;
    for (int i = 1; i <= 1000000; i++) {
        int *fds = malloc(100 * sizeof(int));
        for (int j = 0; j < 100; j++)
            fds[j] = i;
        s += fds[0];
        free(fds);
    }
    printf("%lld\n", s);
    return 0;
}
