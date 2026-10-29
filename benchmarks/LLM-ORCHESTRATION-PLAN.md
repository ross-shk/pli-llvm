# LLM Orchestration Benchmarks — Plan

Add CPU-side benchmarks for the work that local LLM runners — the
[Strata](https://github.com/Niko1221/Strata) MoE engine first, plus
llama.cpp / ollama and vLLM — do *around* the GPU kernels: tokenization,
sampling, KV-cache bookkeeping, grammar masks, Strata's adaptive expert
tier and its prompt-lookup drafter. Each workload is a PL/I program plus a C
twin that does the same work, registered in `benchmarks/run_benchmarks.py`
like the rest of the `ir/` corpus (see `benchmarks/README.md`).

All six algorithms below have been prototyped with the current compiler
(arm64 macOS, `-O2`): each PL/I program prints the same output as its C
twin, and also runs clean with `--size-checks --subscript-checks`. The
drafts are in `examples/out/llm/` (git-ignored scratch) and can be used as
the starting point for P1–P6.

| Benchmark | Host work it prices | Runner it follows |
|---|---|---|
| `llm_tokenize` | BPE merge loop | llama.cpp (Strata reuses ggml/llama.cpp parts) |
| `llm_sample` | penalties, top-k, seeded pick | Strata's sampler (top_k ≤ 64, default 20), llama.cpp |
| `llm_kvcache` | paged KV blocks, prefix sharing, copy-on-write | vLLM (Strata exposes vLLM's KV/prefix-cache metrics) |
| `llm_grammar` | DFA token mask | llama.cpp GBNF — Strata does *not* do this (below) |
| `llm_expert_tier` | decayed expert counts, miss split, hot/cold swaps | Strata's adaptive VRAM expert tier (`--adapt-every/-swaps/-decay`, `--pcie-frac`) |
| `llm_lookup` | n-gram index, suffix match, draft + verify | Strata's prompt-lookup drafter (`--lookup-chain`, min match 3, up to 5 drafts) |

Strata's own docs say its JSON output is "schema prompting followed by
server validation, not grammar-constrained decoding", so `llm_grammar`
stays as the llama.cpp-style row and is not a Strata claim.

## 1. Scope: the GPU line

**Out of scope:** anything that runs on the GPU — matmul, attention, RoPE,
weight dequantization, CUDA kernels. This compiler does not generate code
for them, and a CPU version would just be another `array_matmul`. For
Strata that also excludes the CPU's *expert computation* (AVX-512/AVX2
quantized matmul over the experts the GPU lacks — dense math, not
orchestration) and the SSD n-gram table reads (I/O, already priced by
`file_io`).

**In scope:** the host-side work around those kernels — hash lookups over a
merge table, scans over a logits array, block-table index chasing, DFA table
walks, expert-count bookkeeping and swap selection, and n-gram suffix
matching. All six are deterministic, integer-only loops, which is what the
PL/I/C ratio in `ir/` measures.

These are measurement programs, not language features: no `src/`,
`runtime/`, `tests/` or `GRAMMAR-COVERAGE.md` change, no TR 25.084 rule
moves, and no ADR is needed (unless P8 adds a lowering knob — see §6).

## 2. Placement and gating

Each workload has a C twin doing identical work, so the signal is the
PL/I/C ratio. They belong in `IR_BENCHMARKS`, not `lang/` (which is for
PL/I-only semantics priced by PL/I-vs-PL/I pairs).

What registration actually does (checked in `run_benchmarks.py`):

- `GATE_BENCHMARKS` is derived from `IR_BENCHMARKS` minus `NON_PARITY`. A
  registered row **is a gate row on the next full run** — there is no
  "registered but not gating" state. During P1–P6 only `--only` runs are
  made, and `--only` never evaluates the gate, so the first full run (P7)
  is where the gate first sees the new rows.
- `validate_inventory()` requires a `CHECKS` entry and both source files
  for every registered row; `tests/driver/bench_harness.sh` runs that check
  in the test suite.
- The regression guard compares only runs with a matching manifest
  (benchmark set + source hashes). Adding rows changes the manifest, so
  the first full run after registration has nothing to compare with — it
  becomes the new reference. This is expected, not a failure.
- Gate headroom: the last recorded IR geomean is **0.75x** over 20 rows
  (`docs/OPTIMIZATION-BASELINE.md` §13). Six new rows at 1.25x each would
  move it to about 0.84x — under the 0.90x gate, but with less room than
  before. A row at 2x or worse would need a decision (P7).

## 3. Shared conventions (all six programs)

These came out of the prototypes; each one avoids a real failure seen
there or in the harness.

| Rule | Why |
|---|---|
| SPDX + copyright header, then a header comment naming the runner algorithm being priced | matches every existing `ir/` file |
| Modern lowercase PL/I, one leading space, nothing past column 72 | repo card-margin rule |
| One array per `DECLARE` — no `dcl (a(10), b(10)) ...` | the factored form with per-item dimensions is rejected (rule (11)) |
| No internal procedure that calls a *sibling* procedure which reads the main procedure's variables | diagnosed as not implemented (rule (8)); see §9 |
| Never rely on short-circuit `&`/`|`: when a test guards a subscript, nest the `if`s | PL/I evaluates both operands, so `best = 0 | x(best) > ...` reads `x(0)` |
| Seeded LCG, written the same way on both sides: `seed = mod(seed * 1103515245 + 12345, 2147483648)`, draw with `mod(seed / 65536, n)` | deterministic, no `rand()`; the high bits avoid the LCG's short low-bit cycles |
| `seed`, checksums and anything that can pass 2^31 are `fixed bin(63)` / `int64_t` | fixed-bin overflow is a hard error in PL/I and UB in C |
| Checksums fold as `chk = mod(chk * 31 + v, 1000000007)` | stays far below 2^63; identical in C with `%` on non-negative values |
| Output: one value per `put skip list(x);`, C prints `"%lld\n"` per value | verified: this gives byte-identical stdout; the harness compares `strip()`ed text exactly |
| C twin uses 1-based arrays (size n+1) and the same loop bounds | keeps the two programs line-for-line comparable for review |
| Ties: strict `>` while scanning upward, so the lowest index wins | deterministic on both sides |
| Size the outer loop so the PL/I `-O2` median is 100–300 ms; `CHECKS` stays `{"min_ms_O2": 50, "float_tol": False}` | every existing row uses 50 (the harness's noise-floor mark, not a gate); `-O0` stays a few seconds |

## 4. The benchmarks

### 4.1 `llm_tokenize` — BPE merge loop

**Prices:** the CPU half of BPE tokenization: for each pre-tokenized word,
repeatedly find the adjacent pair with the lowest merge rank (hash-table
lookup, as llama.cpp's `bpe_ranks` map does), replace it with the merged
token id and close the gap.

**Changes from v1 of this plan:** v1 blanked the two bytes instead of
merging them, so it never produced a merged token, and it searched a
linear vocab table. The new version merges into a new id and uses an
open-addressing hash table — the real shape of the work.

Data:

- alphabet: ids 1..8; word length 4..15 drawn from the LCG;
- merge table (open addressing, 4096 slots, key `left * 4096 + right`,
  hash `mod(key * 2654435761, 4096) + 1`, linear probing, key 0 = empty):
  - level 1: all 64 letter pairs → ids 9..72, rank
    `mod(i * 37 + j * 11, 64) + 1`;
  - level 2: (pair id, letter) where `mod(i + j, 3) = 0` → next ids, rank
    `100 + mod(i * 13 + j * 7, 512)`.

Hot loop (verified):

```pli
       /* merge the lowest-rank adjacent pair until none is left */
       found = '1'b;
       do while (found);
          best_i = 0;
          best_r = 2147483647;
          do i = 1 to len - 1;
             key = w(i) * 4096 + w(i + 1);
             h = mod(key * 2654435761, 4096) + 1;
             do while (hkey(h) ^= 0 & hkey(h) ^= key);
                h = mod(h, 4096) + 1;
             end;
             if hkey(h) = key then
                if hrank(h) < best_r then do;
                   best_r = hrank(h);
                   best_i = i;
                end;
          end;
          if best_i = 0 then found = '0'b;
          else do;
             /* ... re-probe the winning pair, w(best_i) = hnew(h),
                shift w(best_i+2 .. len) left by one, len = len - 1 */
          end;
       end;
```

Output: total merges, then a position-weighted checksum of every word's
final ids. Prototype: 400 000 words → `1744998` / `130699528` on both
sides, PL/I ≈ 0.24 s, C ≈ 0.32 s (single run, indicative).

### 4.2 `llm_sample` — repetition penalty, top-k, seeded pick

**Prices:** the sampling step between forward passes, as Strata's sampler
does it: a repetition penalty over the vocabulary, top-k selection by
repeated max scans (Strata keeps at most 64 candidates, default 20), and
a seeded weighted pick among the k survivors. Strata applies the penalties
to every token the speculative check verifies, so this runs once per
verified token, not once per forward pass.

**Changes from v1:** v1 started each max scan at `best = 1`, so once token
1 was taken it could be picked again (duplicate top-k entries); it
assigned `0` to a flag it tested as a bit; its "argmax" was always
`order(1)`; and the penalty divided negative logits, which *raises* them.
The new version follows llama.cpp's `repeat_penalty` (which Strata
accepts): divide positive logits, multiply negative ones, then pick by
weight.

Data: `vocab_n = 32000`, `top_k = 20`, `pen = 2`; each step draws fresh
logits in −10000..10000 from the LCG (stand-in for the GPU output);
`seen(i)` counts how often token i was chosen.

Hot loop (verified):

```pli
       /* repetition penalty: push every seen token toward zero */
       do i = 1 to vocab_n;
          if seen(i) > 0 then
             if logits(i) > 0 then logits(i) = logits(i) / pen;
             else logits(i) = logits(i) * pen;
       end;
       /* top-k: take the largest live logit k times (lowest index
          wins a tie) */
       do k = 1 to top_k;
          best = 0;
          do i = 1 to vocab_n;
             if live(i) then
                if best = 0 then best = i;
                else if logits(i) > logits(best) then best = i;
          end;
          live(best) = '0'b;
          order(k) = best;
       end;
       /* seeded pick: weight = logit - k-th logit + 1; the first
          cumulative weight past the target wins */
```

Integer division truncates toward zero on both sides; the prototype
confirms identical results. Output: one checksum of the chosen tokens.
Prototype: 300 steps → `132811396` on both sides, PL/I ≈ 0.87 s,
C ≈ 0.72 s wall (with `top_k = 40` it was 1.00 s vs 0.76 s user) —
**about 1.2–1.3x**. Use ~100 steps for the 100–300 ms target, and
in P2 read the `-emit-llvm` output of the top-k scan: the likely suspect
is the `bit(1)` `live` array (C uses `unsigned char`). `llm_expert_tier`
(§4.5) shows the same gap with a `bit(1)` array, which supports that
suspicion (`docs/DEFERRED-WORK.md` D7).

### 4.3 `llm_kvcache` — paged KV block manager

**Prices:** vLLM-style block management: a free-block stack, per-sequence
logical→physical block tables, reference counts, prefix sharing on fork,
copy-on-write of a shared partial block, and preemption when memory runs
out.

**Changes from v1:** v1 indexed the physical `owner` table with the
logical block number, overwrote `b` without recording the mapping (so the
next token in the same block missed again), started at block 0 of a
1-based array, never freed anything (the free list ran dry), used an
undefined `slot`, and named LRU eviction without implementing it.

Data: 32 sequences, 512 physical blocks of 16 tokens, `btab(32, 64)`.
Each sequence grows to a LCG-drawn goal; when it gets there it is
released and re-forked from its neighbour, sharing up to 120 tokens (7½
blocks, so the last shared block is partial and the first append
triggers copy-on-write). When the free stack is empty, the sequence with
the most blocks (other than the current one) is preempted: all its blocks
are released and it restarts from length 0.

Hot loop (verified):

```pli
          /* append one token: a new logical block, or a shared one
             that must be copied before it is written */
          lb = slen(s) / 16 + 1;
          off = mod(slen(s), 16);
          need = lb > nblk(s);
          cow = '0'b;
          if ^need then do;
             b = btab(s, lb);
             if refs(b) > 1 then do;
                refs(b) = refs(b) - 1;
                cow = '1'b;
                need = '1'b;
             end;
          end;
          if need then do;
             /* no free block: preempt the longest other sequence */
             do while (nfree = 0);
                /* pick victim (most blocks, lowest index), then */
                call release(victim);
                npreempt = npreempt + 1;
             end;
             b = free_stk(nfree);
             nfree = nfree - 1;
             refs(b) = 1;
             btab(s, lb) = b;
             nblk(s) = lb;
          end;
          slen(s) = slen(s) + 1;
          chk = mod(chk * 31 + (b - 1) * 16 + off, 1000000007);
```

Allocation is inline in the main loop because a separate `alloc_blk`
procedure calling the sibling `release` hits the rule (8) limitation.

Output: allocations, copy-on-writes, preemptions, slot checksum.
Prototype: 400 000 ticks × 32 sequences → `822161 / 476 / 26230 /
784993904` on both sides, PL/I ≈ 0.08 s, C ≈ 0.10 s. Use ~1 000 000
ticks. In P3, tune so every counter is clearly non-zero (copy-on-write is
currently ~0.06 % of allocations; 1024 physical blocks would cut the
preemption churn that keeps resetting the fork source).

Strata itself is not a paged-block server: it keeps the KV cache's
most-read part in VRAM and streams the rest from RAM (`--kv-streaming`),
and reuses a conversation's prefix so follow-ups read only the new tokens.
The prefix-sharing and copy-on-write parts of this workload are the
host-side bookkeeping that reuse needs; the block table is vLLM's shape.

### 4.4 `llm_grammar` — DFA token mask

**Prices:** grammar-constrained decoding without a precomputed index
(llama.cpp-style): at every step, walk every vocabulary token's bytes
through a byte-level DFA from the current state; a token is allowed when
its walk never reaches the dead state. Then pick the best allowed token
and advance the grammar state on it.

**Changes from v1:** v1 enabled a token when its *next state was
accepting*, which is not what a mask means (the right test is "not dead");
it tracked a set of live states, which is NFA territory; and its tokens
were single symbols, so there was no per-token walk — the part that
actually costs time.

Data: 32 states × 64 symbols (`trans(32, 0:63)`, about one symbol in five
leads to the dead state 0); 8000 tokens of 1..8 symbols in one flat
`pool`, addressed by `tstart`/`tlen`; the score of a token is a cheap
hash of (token, step), standing in for the logits.

Hot loop (verified; `to ... while` is rule (71), served):

```pli
       do tok = 1 to vocab_n;
          st = s;
          do j = tstart(tok) to tstart(tok) + tlen(tok) - 1
             while (st ^= 0);
             st = trans(st, pool(j));
          end;
          if st ^= 0 then do;
             nallow = nallow + 1;
             score = mod(tok * 7919 + step * 104729, 65536);
             if score > best_score then do;
                best_score = score;
                best = tok;
             end;
          end;
       end;
```

When no token is allowed the grammar restarts at state 1. Output: total
allowed tokens across all steps, then a checksum of (chosen token, new
state). Prototype: 3000 steps → `10023026` / `148437306` on both sides,
PL/I ≈ 0.33 s, C ≈ 0.26 s (≈1.27x). Use ~2000 steps.

### 4.5 `llm_expert_tier` — Strata's adaptive expert tier

**Prices:** the bookkeeping that decides which MoE experts live in VRAM.
Qwen3.8-Flash-Next has 48 layers × 512 experts (24,576), 10 routed per
token per layer. Strata counts every routed expert, sends a miss either to
the CPU pool or over PCIe (`--pcie-frac`), and every few windows decays the
counts and swaps a few hot outsiders in for cold residents
(`--adapt-every`, `--adapt-swaps`, `--adapt-decay`; its RAM tier uses
"decayed count 2 or more" in, "below 0.5" out, largest difference first).
The router itself runs on the GPU; its output is drawn from the LCG here.

Data: `cnt(24576)` decayed counts in fixed point (16 = one hit, so 32 is
2.0 and 8 is 0.5), `res(24576)` `bit(1)` residency, 64 residents per layer
(a 12 GB card holds a few thousand experts). Routing is skewed toward low
ids (`u * u / 512`) and drifts to a new "topic" every 2000 tokens, so the
hot set really moves. Misses go over PCIe 3 times in 10, else to the CPU.
Every 8 tokens: decay `cnt = cnt - cnt / 8`, then up to 4 swaps per layer.

Hot loop (verified):

```pli
          /* route 10 experts, skewed toward the topic's favourites */
          do k = 1 to 10;
             seed = mod(seed * 1103515245 + 12345, 2147483648);
             u = mod(seed / 65536, 512);
             e = mod(u * u / 512 + topic * 37 + l, 512) + 1;
             cnt(base + e) = cnt(base + e) + 16;
             if res(base + e) then hits = hits + 1;
             /* a miss goes over PCIe 3 times in 10, else to the CPU */
             else if mod(cpu_miss + pcie_miss, 10) < 3 then
                pcie_miss = pcie_miss + 1;
             else cpu_miss = cpu_miss + 1;
          end;
```

and the swap pass, once per layer per adapt window:

```pli
             do sw = 1 to 4;
                /* one scan finds the hottest outsider and the coldest
                   resident (lowest index wins a tie) */
                ...
                if cnt(base + hot) >= 32 then
                   if cnt(base + cold) < 8 then do;
                      res(base + hot) = '1'b;
                      res(base + cold) = '0'b;
                      swaps = swaps + 1;
                   end;
             end;
```

Output: hits, CPU misses, PCIe misses, swaps, resident count (must stay
3072) and a checksum of the final resident set. Prototype: 40 000 tokens
→ `6528830 / 8869819 / 3801351 / 74404 / 3072 / 748913249` on both sides
(a 34 % hit rate), PL/I ≈ 1.35 s, C ≈ 1.07 s (≈1.26x — the second
`bit(1)`-array row above 1.2x). Use ~8000 tokens.

### 4.6 `llm_lookup` — Strata's prompt-lookup drafter

**Prices:** speculative drafting from the context. When a reply repeats
earlier text (code edits, quotes), Strata's prompt lookup matches the last
tokens against the context (`--lookup-chain-min`, default 3), drafts up to
5 tokens that followed the match, and the model verifies them in one pass,
keeping the agreeing prefix plus one token of its own. The n-gram index
and the match are host work; the verify pass is GPU work, replaced here by
a comparison with the known target text.

Data: a 400 000-token target text of fresh runs (8..40 random ids out of
32 000) mixed with copies of earlier spans (60 % of runs), as in a code
edit. A trigram hash index (`head(65536)` + `prev(n)` chains) is extended
as decoding moves forward. A lookup walks up to 8 candidates and keeps
the one with the longest backward match (≥ 3, up to 16).

Hot loop (verified; both inner loops use `to ... while`):

```pli
       do while (cand > 0 & walk < 8);
          walk = walk + 1;
          m = 0;
          do j = 0 to 15 while (m = j);
             if cand - j >= 1 then
                if tgt(cand - j) = tgt(pos - j) then m = m + 1;
          end;
          if m >= 3 then
             if m > best_len then do;
                best_len = m;
                best = cand;
             end;
          cand = prev(cand);
       end;
       /* draft up to five tokens after the match; accept the prefix
          the target agrees with, plus the model's own next token */
       ...
          do j = 1 to nd while (acc = j - 1);
             if tgt(best + j) = tgt(pos + j) then acc = acc + 1;
          end;
       ...
       pos = pos + acc + 1;
```

Output: decode steps, drafted tokens, accepted tokens, checksum.
Prototype: one 400 000-token text → `231515 / 247503 / 168476 /
594160705` on both sides (68 % of drafts accepted — in the 0.5–0.8 band
Strata's docs report for its drafts), PL/I ≈ C ≈ 0.02 s. One text is too fast:
P6 wraps it in an outer loop of ~8 "requests", each with its own seed
and a fresh index, for the 100–300 ms target.

## 5. File plan

| Path | Action | Content |
|---|---|---|
| `benchmarks/ir/llm_tokenize.pli` | create | PL/I workload (§4.1) |
| `benchmarks/ir/baseline/llm_tokenize_c.c` | create | C twin |
| `benchmarks/ir/llm_sample.pli` | create | PL/I workload (§4.2) |
| `benchmarks/ir/baseline/llm_sample_c.c` | create | C twin |
| `benchmarks/ir/llm_kvcache.pli` | create | PL/I workload (§4.3) |
| `benchmarks/ir/baseline/llm_kvcache_c.c` | create | C twin |
| `benchmarks/ir/llm_grammar.pli` | create | PL/I workload (§4.4) |
| `benchmarks/ir/baseline/llm_grammar_c.c` | create | C twin |
| `benchmarks/ir/llm_expert_tier.pli` | create | PL/I workload (§4.5) |
| `benchmarks/ir/baseline/llm_expert_tier_c.c` | create | C twin |
| `benchmarks/ir/llm_lookup.pli` | create | PL/I workload (§4.6) |
| `benchmarks/ir/baseline/llm_lookup_c.c` | create | C twin |
| `benchmarks/run_benchmarks.py` | edit | `IR_BENCHMARKS` +6 rows (with a one-line comment for the group), `CHECKS` +6 rows |
| `docs/OPTIMIZATION-BASELINE.md` | edit | new section "17. LLM orchestration workloads": per-row ratios, geomean before/after, commands, run id |
| `docs/DEFERRED-WORK.md` | done | Band D row D7 for the rows above 1.0x (already added; P7 fills in the measured numbers) |

Registration (appended at the end of `IR_BENCHMARKS`):

```python
    # Host-side LLM runner work around the GPU kernels: llama.cpp, vLLM
    # and Strata (benchmarks/LLM-ORCHESTRATION-PLAN.md).
    ("llm_tokenize",      "ir/llm_tokenize.pli",      "ir/baseline/llm_tokenize_c.c"),
    ("llm_sample",        "ir/llm_sample.pli",        "ir/baseline/llm_sample_c.c"),
    ("llm_kvcache",       "ir/llm_kvcache.pli",       "ir/baseline/llm_kvcache_c.c"),
    ("llm_grammar",       "ir/llm_grammar.pli",       "ir/baseline/llm_grammar_c.c"),
    ("llm_expert_tier",   "ir/llm_expert_tier.pli",   "ir/baseline/llm_expert_tier_c.c"),
    ("llm_lookup",        "ir/llm_lookup.pli",        "ir/baseline/llm_lookup_c.c"),
```

```python
    "llm_tokenize":     {"min_ms_O2": 50, "float_tol": False},
    "llm_sample":       {"min_ms_O2": 50, "float_tol": False},
    "llm_kvcache":      {"min_ms_O2": 50, "float_tol": False},
    "llm_grammar":      {"min_ms_O2": 50, "float_tol": False},
    "llm_expert_tier":  {"min_ms_O2": 50, "float_tol": False},
    "llm_lookup":       {"min_ms_O2": 50, "float_tol": False},
```

No `EXTRA_PLIFLAGS`, `FLOAT_BENCHES`, `BENCHMARKS_REQUIRING_MATH` or
`NON_PARITY` entries: the programs need no checks forced, no floats and no
libm, and all six are exact-parity twins. `benchmarks/README.md` needs no
change (it describes corpora, not individual rows).

## 6. Phased steps

**P0 — Baseline the harness.** `cmake --build build/cmake -j` (fresh
build), then `python3 benchmarks/run_benchmarks.py`. Record the IR
geomean and the run id. Done when the run is green and the number is
written down.

**P1 — `llm_tokenize`.** Copy the draft from `examples/out/llm/`, add the
header, check margins (`awk 'length > 72'`), register both rows, then:

```sh
python3 benchmarks/run_benchmarks.py --only llm_tokenize --runs 7
python3 benchmarks/run_benchmarks.py --only llm_tokenize --checks-forced
```

Done when: parity is `✓` at every `-O` level, the `-O2` PL/I median is
100–300 ms, the checks-forced run prints identical output, and
`python3 -m unittest discover -s benchmarks/tests -p 'test_*.py'` passes.
If the ratio is above 1.1x, read the `-emit-llvm` output of the hot loop
and note the cause (a note, not a fix — fixes are separate work).

**P2 — `llm_sample`.** Same steps. Expect ~1.2–1.3x; the IR check of the
`bit(1)` `live` array is part of this phase (evidence for D7).

**P3 — `llm_kvcache`.** Same steps, plus the counter tuning in §4.3.

**P4 — `llm_grammar`.** Same steps.

**P5 — `llm_expert_tier`.** Same steps. Also check that the resident count
prints 3072 at every `-O` level (a swap that lost or duplicated a resident
would change it). Expect ~1.26x; compare its IR with P2's — if both gaps
come from the `bit(1)` array, D7 has one cause, not two.

**P6 — `llm_lookup`.** Add the outer "requests" loop first (§4.6), then
the same steps.

**P7 — Corpus gate.** Fresh build, full `python3
benchmarks/run_benchmarks.py`. Confirm `26/26 rows valid` and the `-O2`
geomean is under 0.90x. If one new row alone pushes the geomean over the
gate, stop and decide with the user: keep it and record the miss, or
move it out of `IR_BENCHMARKS`. Write section 17 of
`docs/OPTIMIZATION-BASELINE.md` and put the measured ratios into D7 in
`docs/DEFERRED-WORK.md` (rows that measure ≤ 1.0x leave D7). Done when
both are written.

**P8 (optional) — Paired variants.** Add `PAIRS` rows only if a real
PL/I-only knob already exists whose two paths both compile these loops.
A *new* `--experimental-lowering=` knob is compiler work and needs its
own plan and ADR.

Commits (each needs approval): one commit with the six benchmarks, their
registration and the D7 row once P1–P6 all pass (the repo rule is that a
deferral lands in the commit that creates it), then one commit with the
baseline numbers from P7.

## 7. Harness contract every new bench must satisfy

- Deterministic: no `rand()`, no time of day, no uninitialized reads,
  fixed iteration counts. The warmup and every timed run print identical
  output (`output_mismatch` otherwise).
- Exact PL/I/C output match (`✓`), integers only.
- Clean under `--checks-forced` (no SIZE overflow, no subscript out of
  range) — this catches index-0 and off-by-one bugs on the PL/I side.
- Each loop's result feeds the checksum, so LLVM cannot delete the work
  on either side.
- Every timed binary is rebuilt by the harness on each run; still rebuild
  the compiler before any timing.

## 8. Review checklist

- [ ] No `src/`, `runtime/` or `tests/` change; platform separation
  untouched.
- [ ] Served syntax only: `DO WHILE` and `DO ... TO ... WHILE` (rule
  (71)), `MOD`, fixed-bin arithmetic, 1-D/2-D arrays, `bit(1)` flags.
  No `||`, `SELECT`, `LEAVE`/`ITERATE`/`EXIT` or `DO UNTIL`.
- [ ] Margins 2–72 and SPDX headers on all twelve files.
- [ ] `validate_inventory()` clean; `bench_harness.sh` passes.
- [ ] Every `--only` run `✓`, every `--checks-forced` run identical.
- [ ] Full run: 26/26 rows valid, gate result recorded in
  `docs/OPTIMIZATION-BASELINE.md` with the run id; D7 carries the
  measured ratios.

## 9. Findings outside this plan

Found while prototyping; worth separate tasks (and `docs/DEFERRED-WORK.md`
entries), not part of this work:

1. **Sibling-procedure limitation crashes after diagnosing.** An internal
   procedure that calls a sibling which reads the main procedure's
   variables gets the rule (8) "not implemented" error, but compilation
   carries on into IRGen and ends in `internal error: LLVM module
   verification failed`. Expected: stop after the diagnostic.
2. **`-fsyntax-only` misses the same case.** It returns 0 with no
   diagnostic, so the gap is only found in IRGen, not sema.
3. **Factored declarations with per-item dimensions** (`dcl (a(10),
   b(10)) fixed bin(31);`) are rejected with rule (11). Check against
   TR 25.084 rule (11) and IBM factoring rules before deciding whether
   this is a gap or correct.

Minimal reproduction for 1 and 2:

```pli
 t: proc options(main);
    dcl n fixed bin(31);
    n = 0;
    call a;
    put skip list(n);
 a: proc;
    call b;
 end a;
 b: proc;
    n = n + 1;
 end b;
 end t;
```

## 10. Settled questions

- **Which runner:** "strate-style" in the first version meant
  [Strata](https://github.com/Niko1221/Strata) (Niko1221's MoE engine).
  §4.5 and §4.6 price its two host-side signatures; §4.2 follows its
  sampler defaults; §4.4 is marked as llama.cpp-only because Strata does
  not constrain decoding with a grammar.
- **Rows above 1.0x:** they get an entry in `docs/DEFERRED-WORK.md` —
  Band D row **D7**, `gated` on the P7 measurement.
