/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
 * C baseline for select_chain: the same twenty constant arms, searched with
 * a binary search over a sorted table, which is what a binary-search
 * lowering of the PL/I SELECT would produce. */
#include <stdio.h>
#include <string.h>

static const char *const arms[20] = {
    "a00", "a01", "a02", "a03", "a04", "a05", "a06", "a07", "a08", "a09",
    "a10", "a11", "a12", "a13", "a14", "a15", "a16", "a17", "a18", "a19"
};

int main(void) {
    int hits = 0;
    for (int i = 1; i <= 20000000; i++) {
        int k = i - (i / 20) * 20;
        char s[4];
        s[0] = k < 10 ? 'a' : 'a';
        s[1] = k < 10 ? '0' : '1';
        s[2] = (char)('0' + k % 10);
        s[3] = 0;
        /* the binary search over the sorted arms */
        int lo = 0, hi = 19, r = -1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            int c = strcmp(s, arms[mid]);
            if (c == 0) { r = mid; break; }
            if (c < 0) hi = mid - 1; else lo = mid + 1;
        }
        if (r == k) hits++;
    }
    printf("%d\n", hits);
    return 0;
}
