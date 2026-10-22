/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
 * C baseline for by_name: the same ten-member structure copied with one
 * aggregate move, which is what a BY NAME assignment could lower to. */
#include <stdio.h>

struct rec {
    int a, b, c, d, e, f, g, h, i, j;
};

int main(void) {
    struct rec src, dst;
    int hits = 0;
    for (int i = 1; i <= 20000000; i++) {
        src.a = i;
        src.b = i + 1;
        src.c = i + 2;
        src.d = i + 3;
        src.e = i + 4;
        src.f = i + 5;
        src.g = i + 6;
        src.h = i + 7;
        src.i = i + 8;
        src.j = i + 9;
        dst = src;
        if (dst.a + dst.j == 2 * i + 9) hits++;
    }
    printf("%d\n", hits);
    return 0;
}
