/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* C baseline for llm_kvcache: the same block table, free stack,
   reference counts, sharing, copy-on-write and preemption
   (benchmarks/LLM-ORCHESTRATION-PLAN.md 4.3). Arrays are 1-based, as on
   the PL/I side, so the loops match. */
#include <stdio.h>
#include <stdint.h>

static int btab[33][65], nblk[33], slen[33], goal[33];
static int refs[321], free_stk[321], nfree;

static void release_seq(int q) {
    for (int k = 1; k <= nblk[q]; k++) {
        int p = btab[q][k];
        if (--refs[p] == 0) free_stk[++nfree] = p;
    }
    nblk[q] = 0;
    slen[q] = 0;
}

int main(void) {
    int s, q, b = 0, lb, off, j, victim, src, nshare, tick, need, cow;
    int64_t seed = 4242, chk = 0, nalloc = 0, ncow = 0, npreempt = 0;

    /* every physical block starts on the free stack */
    nfree = 0;
    for (b = 320; b >= 1; b--) { free_stk[++nfree] = b; refs[b] = 0; }
    for (s = 1; s <= 32; s++) {
        nblk[s] = 0;
        slen[s] = 0;
        seed = (seed * 1103515245 + 12345) % 2147483648LL;
        goal[s] = 32 + (int)((seed / 65536) % 224);
    }
    for (tick = 1; tick <= 1200000; tick++) {
        if (tick % 5000 == 0) {
            victim = 0;
            for (q = 2; q <= 32; q++) {
                if (victim == 0) victim = q;
                else if (nblk[q] > nblk[victim]) victim = q;
            }
            if (nblk[victim] > 0) {
                release_seq(victim);
                npreempt++;
            }
        }
        for (s = 1; s <= 32; s++) {
            if (slen[s] >= goal[s]) {
                release_seq(s);
                src = s % 32 + 1;
                slen[s] = slen[src];
                if (slen[s] > 120) slen[s] = 120;
                nshare = (slen[s] + 15) / 16;
                for (j = 1; j <= nshare; j++) {
                    btab[s][j] = btab[src][j];
                    refs[btab[s][j]]++;
                }
                nblk[s] = nshare;
                seed = (seed * 1103515245 + 12345) % 2147483648LL;
                goal[s] = slen[s] + 32 + (int)((seed / 65536) % 224);
            }
            lb = slen[s] / 16 + 1;
            off = slen[s] % 16;
            need = lb > nblk[s];
            cow = 0;
            if (!need) {
                b = btab[s][lb];
                if (refs[b] > 1) { refs[b]--; cow = 1; need = 1; }
            }
            if (need) {
                while (nfree == 0) {
                    victim = 0;
                    for (q = 1; q <= 32; q++)
                        if (q != s) {
                            if (victim == 0) victim = q;
                            else if (nblk[q] > nblk[victim]) victim = q;
                        }
                    release_seq(victim);
                    npreempt++;
                }
                b = free_stk[nfree--];
                refs[b] = 1;
                btab[s][lb] = b;
                nblk[s] = lb;
                nalloc++;
                if (cow) ncow++;
            }
            slen[s]++;
            chk = (chk * 31 + (int64_t)(b - 1) * 16 + off) % 1000000007;
        }
    }
    printf("%lld\n%lld\n%lld\n%lld\n", (long long)nalloc, (long long)ncow,
           (long long)npreempt, (long long)chk);
    return 0;
}
