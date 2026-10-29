/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* C baseline for llm_expert_tier: the same counters, miss split, decay
   and hot/cold swaps (benchmarks/LLM-ORCHESTRATION-PLAN.md 4.5).
   Arrays are 1-based, as on the PL/I side, so the loops match. */
#include <stdio.h>
#include <stdint.h>

static int cnt[24577];
static unsigned char res[24577];

int main(void) {
    int l, k, e, base, i, tok, topic = 0, sw, hot, cold, nres = 0;
    int64_t seed = 2026, u, hits = 0, cpu_miss = 0, pcie_miss = 0;
    int64_t swaps = 0, chk = 0;

    for (l = 0; l <= 47; l++)
        for (e = 1; e <= 512; e++) {
            cnt[l * 512 + e] = 0;
            res[l * 512 + e] = e <= 64;
        }
    for (tok = 1; tok <= 8000; tok++) {
        if (tok % 2000 == 0) topic++;
        for (l = 0; l <= 47; l++) {
            base = l * 512;
            for (k = 1; k <= 10; k++) {
                seed = (seed * 1103515245 + 12345) % 2147483648LL;
                u = (seed / 65536) % 512;
                e = (int)((u * u / 512 + topic * 37 + l) % 512) + 1;
                cnt[base + e] += 16;
                if (res[base + e]) hits++;
                else if ((cpu_miss + pcie_miss) % 10 < 3) pcie_miss++;
                else cpu_miss++;
            }
        }
        if (tok % 8 == 0)
            for (l = 0; l <= 47; l++) {
                base = l * 512;
                for (e = 1; e <= 512; e++)
                    cnt[base + e] = cnt[base + e] - cnt[base + e] / 8;
                for (sw = 1; sw <= 4; sw++) {
                    hot = 0;
                    cold = 0;
                    for (e = 1; e <= 512; e++) {
                        if (res[base + e]) {
                            if (cold == 0) cold = e;
                            else if (cnt[base + e] < cnt[base + cold]) cold = e;
                        } else {
                            if (hot == 0) hot = e;
                            else if (cnt[base + e] > cnt[base + hot]) hot = e;
                        }
                    }
                    if (cnt[base + hot] >= 32 && cnt[base + cold] < 8) {
                        res[base + hot] = 1;
                        res[base + cold] = 0;
                        swaps++;
                    }
                }
            }
    }
    for (i = 1; i <= 24576; i++)
        if (res[i]) {
            nres++;
            chk = (chk * 31 + i) % 1000000007;
        }
    printf("%lld\n%lld\n%lld\n%lld\n%d\n%lld\n", (long long)hits,
           (long long)cpu_miss, (long long)pcie_miss, (long long)swaps,
           nres, (long long)chk);
    return 0;
}
