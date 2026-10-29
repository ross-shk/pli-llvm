/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* C baseline for llm_lookup: the same trigram index, suffix match and
   draft/verify loop, over the same twelve seeded texts
   (benchmarks/LLM-ORCHESTRATION-PLAN.md 4.6). Arrays are 1-based, as
   on the PL/I side, so the loops match. */
#include <stdio.h>
#include <stdint.h>

static int tgt[400101], head[65537], prev[400101];

int main(void) {
    int n = 400000, nreq = 12, req, i, j, len, src, pos, best, best_len;
    int cand, walk, m, nd, acc, h, done;
    int64_t seed, steps = 0, drafted = 0, accepted = 0, chk = 0;

    for (req = 1; req <= nreq; req++) {
        /* the text to be decoded: fresh runs mixed with copies */
        seed = 31337 + (int64_t)req * 7919;
        i = 0;
        while (i < n) {
            seed = (seed * 1103515245 + 12345) % 2147483648LL;
            len = 8 + (int)((seed / 65536) % 33);
            seed = (seed * 1103515245 + 12345) % 2147483648LL;
            if (i > 1000 && (seed / 65536) % 10 < 6) {
                seed = (seed * 1103515245 + 12345) % 2147483648LL;
                src = 1 + (int)((seed / 65536) % (i - 50));
                for (j = 0; j <= len - 1; j++)
                    if (i < n) { i++; tgt[i] = tgt[src + j]; }
            } else {
                for (j = 1; j <= len; j++)
                    if (i < n) {
                        i++;
                        seed = (seed * 1103515245 + 12345) % 2147483648LL;
                        tgt[i] = 1 + (int)((seed / 65536) % 32000);
                    }
            }
        }
        /* a fresh trigram index for this request */
        for (i = 1; i <= 65536; i++) head[i] = 0;

        pos = 3;
        done = 2;
        while (pos < n - 6) {
            while (done < pos - 1) {
                done++;
                h = (tgt[done - 2] * 961 + tgt[done - 1] * 31
                     + tgt[done]) % 65536 + 1;
                prev[done] = head[h];
                head[h] = done;
            }
            h = (tgt[pos - 2] * 961 + tgt[pos - 1] * 31 + tgt[pos])
                % 65536 + 1;
            cand = head[h];
            best = 0;
            best_len = 0;
            walk = 0;
            while (cand > 0 && walk < 8) {
                walk++;
                m = 0;
                for (j = 0; j <= 15 && m == j; j++)
                    if (cand - j >= 1 && tgt[cand - j] == tgt[pos - j]) m++;
                if (m >= 3 && m > best_len) {
                    best_len = m;
                    best = cand;
                }
                cand = prev[cand];
            }
            nd = 0;
            acc = 0;
            if (best > 0) {
                nd = 5;
                if (best + nd > pos) nd = pos - best;
                for (j = 1; j <= nd && acc == j - 1; j++)
                    if (tgt[best + j] == tgt[pos + j]) acc++;
            }
            drafted += nd;
            accepted += acc;
            pos = pos + acc + 1;
            steps++;
            chk = (chk * 31 + (int64_t)best * 8 + acc) % 1000000007;
        }
    }
    printf("%lld\n%lld\n%lld\n%lld\n", (long long)steps, (long long)drafted,
           (long long)accepted, (long long)chk);
    return 0;
}
