/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* C baseline for io_throughput benchmark */
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    int i, total;
    total = 0;
    for (i = 1; i <= 1000; i++) {
        printf("%d %d %d\n", i, i * i, abs(5000 - i * 3));
        total += i;
    }
    printf("%d\n", total);
    return 0;
}
