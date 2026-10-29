/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* C baseline for llm_sample: the same penalties, top-k scans and seeded
   pick over the same logits (benchmarks/LLM-ORCHESTRATION-PLAN.md 4.2).
   Arrays are 1-based, as on the PL/I side, so the loops match. */
#include <stdio.h>
#include <stdint.h>

static int logits[32001], seen[32001];
static unsigned char live[32001];

int main(void) {
    int order[65];
    int i, k, best, step, chosen, lo;
    int vocab_n = 32000, top_k = 20, pen = 2, nsteps = 100;
    int64_t seed = 777, total, acc, target, chk = 0;

    for (i = 1; i <= vocab_n; i++) seen[i] = 0;
    for (step = 1; step <= nsteps; step++) {
        for (i = 1; i <= vocab_n; i++) {
            seed = (seed * 1103515245 + 12345) % 2147483648LL;
            logits[i] = (int)((seed / 65536) % 20001) - 10000;
            live[i] = 1;
        }
        for (i = 1; i <= vocab_n; i++)
            if (seen[i] > 0) {
                if (logits[i] > 0) logits[i] = logits[i] / pen;
                else logits[i] = logits[i] * pen;
            }
        for (k = 1; k <= top_k; k++) {
            best = 0;
            for (i = 1; i <= vocab_n; i++)
                if (live[i]) {
                    if (best == 0) best = i;
                    else if (logits[i] > logits[best]) best = i;
                }
            live[best] = 0;
            order[k] = best;
        }
        lo = logits[order[top_k]];
        total = 0;
        for (k = 1; k <= top_k; k++) total += logits[order[k]] - lo + 1;
        seed = (seed * 1103515245 + 12345) % 2147483648LL;
        target = seed % total;
        acc = 0;
        chosen = 0;
        for (k = 1; k <= top_k; k++) {
            acc += logits[order[k]] - lo + 1;
            if (chosen == 0 && acc > target) chosen = order[k];
        }
        seen[chosen]++;
        chk = (chk * 31 + chosen) % 1000000007;
    }
    printf("%lld\n", (long long)chk);
    return 0;
}
