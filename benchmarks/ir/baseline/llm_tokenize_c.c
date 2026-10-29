/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* C baseline for llm_tokenize: the same BPE merge loop over the same
   seeded words and merge table (benchmarks/LLM-ORCHESTRATION-PLAN.md
   4.1). Arrays are 1-based, as on the PL/I side, so the loops match. */
#include <stdio.h>
#include <stdint.h>

static int64_t hkey[4097];
static int hrank[4097], hnew[4097];

static void put_merge(int l, int r, int rank, int id) {
    int64_t k = (int64_t)l * 4096 + r;
    int64_t h = (k * 2654435761LL) % 4096 + 1;
    while (hkey[h] != 0) h = h % 4096 + 1;
    hkey[h] = k;
    hrank[h] = rank;
    hnew[h] = id;
}

int main(void) {
    int w[17];
    int i, j, n, len, best_i, best_r, r, nid = 8, nmerge = 0;
    int nwords = 300000;
    int64_t seed = 12345, key, h, chk = 0;

    for (i = 1; i <= 4096; i++) hkey[i] = 0;
    /* level 1: every letter pair merges */
    for (i = 1; i <= 8; i++)
        for (j = 1; j <= 8; j++) {
            nid++;
            r = (i * 37 + j * 11) % 64 + 1;
            put_merge(i, j, r, nid);
        }
    /* level 2: a pair token followed by a letter, a third of them */
    for (i = 9; i <= 72; i++)
        for (j = 1; j <= 8; j++)
            if ((i + j) % 3 == 0) {
                nid++;
                r = 100 + (i * 13 + j * 7) % 512;
                put_merge(i, j, r, nid);
            }

    for (n = 1; n <= nwords; n++) {
        seed = (seed * 1103515245 + 12345) % 2147483648LL;
        len = 4 + (int)((seed / 65536) % 12);
        for (i = 1; i <= len; i++) {
            seed = (seed * 1103515245 + 12345) % 2147483648LL;
            w[i] = 1 + (int)((seed / 65536) % 8);
        }
        for (;;) {
            best_i = 0;
            best_r = 2147483647;
            for (i = 1; i <= len - 1; i++) {
                key = (int64_t)w[i] * 4096 + w[i + 1];
                h = (key * 2654435761LL) % 4096 + 1;
                while (hkey[h] != 0 && hkey[h] != key) h = h % 4096 + 1;
                if (hkey[h] == key && hrank[h] < best_r) {
                    best_r = hrank[h];
                    best_i = i;
                }
            }
            if (best_i == 0) break;
            key = (int64_t)w[best_i] * 4096 + w[best_i + 1];
            h = (key * 2654435761LL) % 4096 + 1;
            while (hkey[h] != key) h = h % 4096 + 1;
            w[best_i] = hnew[h];
            for (i = best_i + 1; i <= len - 1; i++) w[i] = w[i + 1];
            len--;
            nmerge++;
        }
        for (i = 1; i <= len; i++)
            chk = (chk * 31 + (int64_t)w[i] * i) % 1000000007;
    }
    printf("%d\n%lld\n", nmerge, (long long)chk);
    return 0;
}
