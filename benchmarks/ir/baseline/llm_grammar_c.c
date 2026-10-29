/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* C baseline for llm_grammar: the same DFA, vocabulary and per-token
   walk (benchmarks/LLM-ORCHESTRATION-PLAN.md 4.4). Arrays are 1-based,
   as on the PL/I side, so the loops match. */
#include <stdio.h>
#include <stdint.h>

static int trans[33][64], tstart[8001], tlen[8001], pool[48001];

int main(void) {
    int s, c, j, st, tok, best, step, nallow, plen = 0, vocab_n = 8000;
    int score, best_score;
    int64_t seed = 99, chk = 0, nmask = 0;

    for (s = 1; s <= 32; s++)
        for (c = 0; c <= 63; c++)
            trans[s][c] = ((s * 7 + c * 13) % 5 == 0) ? 0
                          : 1 + (s * 11 + c * 3) % 32;
    for (tok = 1; tok <= vocab_n; tok++) {
        seed = (seed * 1103515245 + 12345) % 2147483648LL;
        tlen[tok] = 1 + (int)((seed / 65536) % 8);
        tstart[tok] = plen + 1;
        for (j = 1; j <= tlen[tok]; j++) {
            seed = (seed * 1103515245 + 12345) % 2147483648LL;
            pool[++plen] = (int)((seed / 65536) % 64);
        }
    }
    s = 1;
    for (step = 1; step <= 2000; step++) {
        nallow = 0;
        best = 0;
        best_score = -1;
        for (tok = 1; tok <= vocab_n; tok++) {
            st = s;
            for (j = tstart[tok];
                 j <= tstart[tok] + tlen[tok] - 1 && st != 0; j++)
                st = trans[st][pool[j]];
            if (st != 0) {
                nallow++;
                score = (tok * 7919 + step * 104729) % 65536;
                if (score > best_score) {
                    best_score = score;
                    best = tok;
                }
            }
        }
        nmask += nallow;
        if (best == 0) s = 1;
        else
            for (j = tstart[best]; j <= tstart[best] + tlen[best] - 1; j++)
                s = trans[s][pool[j]];
        chk = (chk * 31 + (int64_t)best * 64 + s) % 1000000007;
    }
    printf("%lld\n%lld\n", (long long)nmask, (long long)chk);
    return 0;
}
