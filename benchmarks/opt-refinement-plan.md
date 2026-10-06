# Benchmark Optimization & Refinement Plan

Status: 2026-10-06. Audit of `benchmarks/` (18 `.pli` + 17 C baselines +
`run_benchmarks.py`, 16 benchmarks wired into the harness). All 16 wired
benchmarks verified by compiling and running each `.pli` against its C
baseline at `-O2`: 15/16 outputs match, `decimal_ops` differs by design
(PL/I per-assignment DECIMAL rounding vs C `double` accumulation — see
DECIMAL-PERF-PLAN.md). The orphan `division` pair also matches, but
fragilely (see B2).

Related: OPTIMIZATION-PLAN.md (P0/P1 codegen work, done), DECIMAL-PERF-PLAN.md
(decimal codegen, done except deferred P4).

Scope: this plan covers benchmark sources and the harness only. No compiler
codegen changes — codegen work stays in the two plans above. Per AGENTS.md:
plan first, one atomic commit per item, no commit without approval.

Global acceptance criteria (apply to every P0 fix and P2 addition):
1. Identical stdout on both sides at `-O0...-O3`, or a documented `≈` with a
   semantic rationale (decimal_ops is the standing example).
2. Slowest `-O2` configuration runs at least 50 ms of compute (spawn < 5% of
   wall) — see P1.3.
3. Runs clean with all checks forced (`--size-checks --subscript-checks`),
   or carries an explicit waiver with justification — see P1.7.

---

## 1. Verification verdict

| Benchmark | PL/I vs C output | Verdict |
| --------- | ---------------- | ------- |
| scalar_arith, array_matmul, array_sort, string_ops, condition_ops, | match (reproduced | correct |
| proc_calls, math_functions, bit_ops, linked_list, collatz, | locally at `-O2`) | |
| controlled_stack, on_conditions, matrix_float, mandelbrot, | | |
| io_throughput | | |
| decimal_ops (`577128133588977.50` vs `577128133588929.75`) | differ | correct-by-design |
| division (orphan, not in harness) | match (`-894831901`) | fragile, see B2 |

Harness facts checked: `BENCHMARKS_REQUIRING_MATH =
{"math_functions", "matrix_float"}` exactly matches the two `.c` files that
`#include <math.h>` — correct. `PLIC = .../build/plic` resolves (symlink to
`build/cmake/plic`) — works. All 16 harness entries have both sources present.

---

## 2. Issues found (ranked)

**B1. `collatz.pli` reuses `n` as both the `DO...TO` bound and the
inner-loop variable.** It only works because the `TO` bound is evaluated
once on loop entry (verified: output `5025114` matches C). The C baseline
correctly uses a separate `m`. One refactor of DO-bound semantics and this
benchmark silently changes meaning.

**B2. `division.pli` / `division_c.c` exist but are orphaned from the
harness, and their checksum is signed-overflow UB.** Both print
`-894831901` — a wrapped-around `int` sum. Any change in
unrolling/vectorization can legally change the C value while PL/I (with SIZE
traps at `-O0`/`-O1`, wraparound at `-O2`/`-O3`) does something else.

**B3. `condition_ops.pli` never fires its handlers — and cannot, at `-O2`/`-O3`.**
100 in-bounds iterations, `x`/`y` peak at 52225/1050 — no SUBSCRIPTRANGE, no
SIZE. Worse, SIZE and SUBSCRIPTRANGE checks are elided by default at `-O2`/`-O3`
(`src/main.cpp:798-803`), so even a deliberately tripping store would not
reach the handler there — it would be a raw out-of-bounds write. The
benchmark as written measures ON-setup overhead plus a trivial loop, and its
`-O3` ratio of **0.24x** (PL/I 4x faster than C) is the tell: at `-O3` there
is effectively nothing left to measure. Fix requires source changes (P0.3)
*plus* per-benchmark forced-checks flags (P1.6); P0.3 without P1.6 would be
a memory-safety bug, not a benchmark.

**B4. All fast benchmarks are startup-noise dominated.** 5-run min/median of
wall time including process spawn (`-O2` binaries, macOS arm64):

| Benchmark | min | median | Benchmark | min | median |
| --------- | --- | ------ | --------- | --- | ------ |
| mandelbrot | 18.36 ms | 18.99 ms | collatz | 9.05 ms | 9.76 ms |
| array_sort | 6.97 ms | 7.65 ms | math_functions | ~7.8 ms | — |
| matrix_float | 4.36 ms | 5.18 ms | array_matmul | 3.63 ms | 4.34 ms |
| controlled_stack | 3.48 ms | 4.27 ms | scalar_arith | 3.06 ms | 3.66 ms |
| bit_ops | 3.13 ms | 3.25 ms | proc_calls | 2.82 ms | 3.19 ms |
| linked_list | 2.95 ms | 4.87 ms | string_ops | 2.92 ms | 3.88 ms |
| decimal_ops | ~3.1 ms | — | condition_ops | ~3.2 ms | — |
| io_throughput | ~3.3 ms | — | on_conditions | ~5.6 ms | — |
| division (orphan) | ~11.9 ms | — | | | |

Process spawn alone is ~2-3 ms, so every benchmark except `mandelbrot`,
`division`, and `collatz` is spawn-dominated. Ratios like `matrix_float -O2
2.31x` / `proc_calls -O3 1.47x` in the latest results JSON are noise — the
same disease DECIMAL-PERF-PLAN.md section 3 diagnosed for `decimal_ops`.
P1.3 scales every workload past the 50 ms rule.

**B5. Harness weaknesses in `run_benchmarks.py`:** `run_benchmark()` is
annotated `-> List[float]` but returns a tuple; compile failures print no
`stdout`/`stderr`; float outputs are compared by exact string equality
(`scalar_arith`'s `9.47481e+226` matching is formatting luck); result binaries
accumulate in `results/`; no CSV export; median-of-5 with no warmup or
outlier rejection.

**B6 (minor).** `mandelbrot` uses escape radius 64.0 (standard is |z|^2 > 4.0 —
consistent on both sides, just undocumented); `string_ops` fills
`CHARACTER(10000) VARYING` to exactly 10000 chars (13-char base + 7-char
`[item]` wrapper = 20 chars x 500 — one byte from truncation/SIZE);
`controlled_stack` compares PL/I CONTROLLED generations against C
`malloc`/`free` (different mechanisms — acceptable, but should be labeled
throughput-not-parity).

**B7. `proc_calls.pli` formal/actual bound mismatch.** `sum_array` declares
its formal as `arr(100)` but is called with 20-element `test_arr`. Only
indices 1..20 are touched, so it is safe today, but the bound lie defeats
any future bounds-checking measurement and invites an overread on the first
edit. Align them (P0.5).

**B8. `on_conditions.pli` is sound — pin it, do not "fix" it.** The
`ON ZERODIVIDE BEGIN; RETURN; END;` unit looks suspicious, but a bare
`RETURN` in an ON-unit is specified resume-after-the-fault (rule 91;
`src/irgen.cpp:2443-2448`, and sema rejects bare `RETURN` in procedure
bodies). Empirically the full 50000-iteration loop completes with `q = 0` on
faulting iterations, matching the C baseline's explicit `q = 0` branch
(`1250235000` both sides). ZERODIVIDE checks, unlike SIZE/SUBSCRIPTRANGE,
are never default-elided, so the handler fires deterministically at every
opt level. The only work needed is a clarifying comment plus a
faulting-rate note (P0.6). Verified FLOAT note: all `FLOAT` lowers to
`double` (`src/irgen.cpp:110-111`), precision attribute ignored — so
`scalar_arith`'s `FLOAT DECIMAL(6)` vs C `double` is double-vs-double, not a
precision mismatch. A future short-float benchmark is meaningless until
codegen distinguishes precisions; recorded as a P2 non-goal until then.

---

## 3. P0 — Correctness fixes (small, do first)

### P0.1 Fix `collatz.pli` loop-variable aliasing

Mirror the C baseline exactly — separate bound `n` from cursor `m`:

```pli
 test_collatz: PROCEDURE OPTIONS(MAIN);
   DECLARE (i, m, n, steps, total) FIXED BINARY(31);
   n = 50000;
   total = 0;
   DO i = 2 TO n;
     steps = 0;
     m = i;
     DO WHILE (m > 1);
       IF MOD(m, 2) = 0 THEN
         m = m / 2;
       ELSE
         m = 3 * m + 1;
       steps = steps + 1;
     END;
     total = total + steps;
   END;
   PUT SKIP LIST(total);
 END test_collatz;
```

Acceptance: output still `5025114`, `✓` at all opt levels.

### P0.2 Integrate `division` into the harness and de-UB the checksum

Register it in `run_benchmarks.py`:

```python
 ("division", "division.pli", "baseline/division_c.c"),
```

Widen both accumulators to 64-bit so the checksum is stable across
`-O0...-O3` and across compilers:

```pli
 DECLARE (i, j, denom, result) FIXED BINARY(31);
 DECLARE sum FIXED BINARY(63);          /* 64-bit accumulator: no wraparound */
 ...
   result = i / denom;
   sum = sum + result;
```

```c
 long sum = 0; /* was: int sum */
 ...
 printf("%ld\n", sum);
```

Acceptance: checksum becomes positive, identical on both sides at all opt
levels before/after.

### P0.3 Make `condition_ops` actually exercise conditions

Split into two timed regions: a clean loop (baseline cost) plus a loop that
deliberately trips SUBSCRIPTRANGE and resumes, so the benchmark measures
handler machinery rather than dead code. Depends on P1.6: the tripping
region is only well-defined with `--subscript-checks` forced at every opt
level (without it, `a(101) = i` at `-O2`/`-O3` is a wild store, see B3).
Prototype first and confirm the `bad` counter is nonzero at all four opt
levels before finalizing; fallback if resume semantics surprise is an
explicit `GOTO` out of the unit.

```pli
 test_conditions: PROCEDURE OPTIONS(MAIN);
   DECLARE (i, n, sum, bad) FIXED BINARY(31);
   DECLARE a(100) FIXED BINARY(31);
   DECLARE (x, y) FIXED BINARY(31);

   ON SUBSCRIPTRANGE
     BEGIN;
       bad = bad + 1;
     END;
   ... /* existing clean loop unchanged (Region 1) */

   /* Region 2: intentional SUBSCRIPTRANGE every 16th iteration.
      Requires --subscript-checks (P1.6); the unit resumes at the
      next statement, skipping the faulting store (rule 91). */
   bad = 0;
   DO i = 1 TO 1600;
     IF MOD(i, 16) = 0 THEN a(101) = i;  /* fires, resumes */
     ELSE a(MOD(i, 100) + 1) = i;
   END;
   PUT SKIP LIST(sum, x, y, bad);
 END test_conditions;
```

Mirror with a C `if`-branch counter so outputs still match:

```c
int bad = 0;
for (i = 1; i <= 1600; i++) {
    if (i % 16 == 0) bad++;              /* handler-cost analogue */
    else a[(i % 100)] = i;
}
printf("%d %d %d %d\n", sum, x, y, bad);
```

SIZE tripping is deliberately *not* included: per-assignment overflow with
forced `--size-checks` aborts differently across opt levels and deserves its
own benchmark (`zerodivide_storm`-style, see P2). Document the `-O3` 0.24x
anomaly as resolved or root-caused. Acceptance: `bad = 100` on both sides at
all opt levels, slowest config past the P1.3 50 ms rule.

### P0.4 Give `string_ops` headroom

`CHARACTER(12000) VARYING` for `result`, keep the 500-iteration workload;
assert `LENGTH(result) = 10000` in both outputs.

```pli
 DECLARE result CHARACTER(12000) VARYING;
```

### P0.5 Align `proc_calls` array bounds

Change the `sum_array` formal from `arr(100)` to `arr(20)` to match the
actual `test_arr(20)` call site (see B7). No behavior change expected —
output stays `522956899` at all levels; the win is that a future
checks-forced run (P1.7) measures a truthful bound pair.

### P0.6 Pin `on_conditions` semantics with a comment

No behavior change (see B8). Add a source comment so the next reader does
not "fix" the bare `RETURN`:

```pli
   /* Bare RETURN ends the ON-unit and resumes after the faulting division
      with q = 0 (rule 91; cf. irgen.cpp zerodivideResume). Every 100th
      iteration faults, so this loop measures ~1% handler-take rate. */
   on zerodivide begin;
      return;
   end;
```

---

## 4. P1 — Harness hardening (`run_benchmarks.py`)

### P1.1 Report compile errors instead of swallowing them

```python
def compile_pli(bench_name, pli_file, opt, output):
    src = BENCHMARK_DIR / pli_file
    cmd = [str(PLIC), str(src), "-o", str(output), opt]
    rc, out, err = run_command(cmd)
    if rc != 0:
        print(f"  PL/I compile failed [{bench_name} {opt}]:\n{out}\n{err}")
    return rc == 0
```

Same for `compile_c`. Also fix the `run_benchmark` return annotation
(`-> Tuple[List[float], str]`).

### P1.2 Warmup + min-of-medians and a fast noise gate

One untimed warmup run, then 7 timed runs; report `min` alongside median
(after P1.3 makes workloads large enough, min approximates steady state):

```python
def run_benchmark(exe, runs=RUNS_PER_BENCH, warmup=1):
    for _ in range(warmup):
        run_command([str(exe)], timeout=60)
    times = []
    ...
    return times, out
```

### P1.3 Scale every benchmark out of the noise floor

Rule: the slowest `-O2` configuration must run at least 50 ms of compute
(spawn < 5% of wall). Current `-O2`-class minima come from the B4 table
(all single-digit ms except `mandelbrot` ~18 ms, `division` ~12 ms,
`collatz` ~9 ms). Concretely, as a starting point (re-measure after, keep
whatever passes the 50 ms rule with the smallest round scaling factor):

| Benchmark | now (min) | Scaling move |
| --------- | --------- | ------------ |
| condition_ops | ~3 ms | 100 -> 20 000 iterations incl. new Region 2 (P0.3) |
| scalar_arith | ~3 ms | Fibonacci 500 -> 5 000 (watch float range: values already reach 1e227; stays `double`, no overflow) |
| string_ops | ~3 ms | concatenation 500 -> 2 000 with the P0.4 12000 headroom raised to 45000, or 4x outer repeat |
| bit_ops | ~3 ms | 100 000 -> 2 000 000 iterations (pure register loop, scales linearly) |
| proc_calls | ~3 ms | factorial/fibonacci driver 12 -> 20 is overflow-limited (12! fits 32-bit, 13! does not); instead wrap the driver loop x50 |
| linked_list | ~3-5 ms | n 5 000 -> 20 000 nodes |
| controlled_stack | ~3-4 ms | n 2 000 -> 10 000 generations |
| decimal_ops | ~3 ms | already at 200 000 (P5); go to 1 000 000 and re-check `DECIMAL(18,2)` headroom (accumulated sum must stay below ~1e16) |
| matrix_float | ~4-5 ms | n 100 -> 200 (8x work: 2-D cubic loop) |
| array_matmul | ~4 ms | n 100 -> 200, checksum must stay in 32-bit (prove the bound first: elements bounded by +-400, products +-160K, row sums +-32M, total +-1.3T overflows — widen checksum to `FIXED BINARY(63)`/`long` as in P0.2) |
| on_conditions | ~6 ms | 50 000 -> 500 000 iterations |
| math_functions | ~8 ms | n 200 -> 400 (doubles the 200K-transcendental inner loop) |
| array_sort | ~7 ms | bubble sort is quadratic: n 2 000 -> 4 000 gives 4x time (~30 ms); prefer n 5 000 (~45 ms) or an outer repeat x8 |
| collatz | ~9 ms | n 50 000 -> 200 000 |
| io_throughput | ~3 ms | do NOT scale by line count alone — pipe + formatting dominate; 1 000 -> 10 000 lines and report throughput (MB/s) alongside wall time |
| mandelbrot | ~18 ms | w/h 600x400 -> 1200x800 (4x pixels, ~75 ms) |

The `decimal_ops` 40x treatment from DECIMAL-PERF-PLAN.md P5 stays the
template. Verify with the P1.2 `min` column that ratios stop swinging more
than 0.2x run-to-run.

### P1.4 Tolerant float comparison

Two problems, in order. First, float benches currently print at ~6
significant digits (list-directed/`%.6g`), so exact string match is both
fragile (a formatting tweak reads as a regression) and weak (it masks real
divergence — `math_functions` accumulates 200 000 transcendental terms, and
6 digits cannot see a ULP-level codegen change). So step one is to raise
printed precision to at least 15 significant digits on both sides (PL/I:
edit-directed E-format or equivalent; C: `%.15g`) for the float-sensitive
benches (`scalar_arith`, `math_functions`, `matrix_float`, `decimal_ops`
informational only since it is `✗`-by-design). Step two is the comparison
itself: exact string equality stays the primary gate; the `≈` tier below
applies only to benches explicitly marked float-sensitive, via token-wise
`isclose`:

```python
import math

FLOAT_BENCHES = {"scalar_arith", "math_functions", "matrix_float"}

def outputs_match(pli_out, c_out, bench_name):
    if pli_out.strip() == c_out.strip():
        return "✓"
    if bench_name not in FLOAT_BENCHES:
        return "✗"
    try:
        ps = [float(t) for t in pli_out.split()]
        cs = [float(t) for t in c_out.split()]
    except ValueError:
        return "✗"
    if len(ps) != len(cs):
        return "✗"
    return "≈" if all(math.isclose(p, c, rel_tol=1e-9) for p, c in zip(ps, cs)) else "✗"
```

Record `match` as `✓/≈/✗` in both the console table and the CSV. Note the
ordering trap: raising precision may turn today's `✓` into `≈` or `✗`
(e.g. `x = x + 0.001` in `math_functions.pli` converts an exact decimal
literal per iteration while C adds binary `0.001` — a legitimate
representation difference). Treat any newly exposed divergence as a finding
to document, not a failure to hide: `≈` with a rationale is a correct
outcome.

### P1.5 Hygiene

Write `results/summary_<ts>.csv` (bench, opt, pli_min, c_min, ratio, match),
skip recompiling unchanged sources via mtime check, stop writing executables
into `results/` (use `results/bin/` and gitignore it), and retain only the
last 20 timestamped JSON/CSV pairs (prune older ones at harness exit):

```python
import csv

with open(RESULTS_DIR / f"summary_{int(time.time())}.csv", "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["benchmark", "opt", "pli_min_ms", "c_min_ms", "ratio", "match"])
    ...
```

### P1.6 Per-benchmark compiler flags

Some benchmarks only mean what they claim with non-default checks. Add an
extra-flags table threaded into `compile_pli` (C side unchanged):

```python
EXTRA_PLIFLAGS = {
    # condition_ops Region 2 must reach its handler at every opt level (B3):
    "condition_ops": ["--subscript-checks"],
}

def compile_pli(bench_name, pli_file, opt, output):
    src = BENCHMARK_DIR / pli_file
    cmd = [str(PLIC), str(src), "-o", str(output), opt,
           *EXTRA_PLIFLAGS.get(bench_name, [])]
    ...
```

Prerequisite for P0.3. Record the active flags per row in the CSV so a ratio
is always read with its check configuration attached. Future use: a
checked-vs-unchecked cost study is one table entry away
(`"condition_ops_unchecked": []` vs `"condition_ops": [...]`).

### P1.7 Checks-forced parity job

Add a CI mode (`--checks-forced`) that compiles every benchmark with
`--size-checks --subscript-checks` at `-O2` and requires identical stdout to
the default run. This is the UB tripwire: it would have caught B2 (signed
wraparound checksum) and catches naive OOB writes (P0.3-without-P1.6 class
of mistake) mechanically. Benches that legitimately wrap carry a waiver in
the plan with justification; the starting assumption is zero waivers, and
each waiver needs its own paragraph. Probe before committing: `collatz`
intermediates (`3 * n + 1`) must be shown to stay in 32-bit range for starts
below 50000 at the P1.3-scaled bound, otherwise it takes the first waiver
or a 63-bit widening.

---

## 5. P2 — Coverage expansion (new benchmarks)

Each ships as a `.pli` + `baseline/*_c.c` pair with identical stdout,
following the `mandelbrot`/`matrix_float` header-comment style, and each
must pass the global acceptance criteria at the top of this plan before it
is wired into `BENCHMARKS` (plus `BENCHMARKS_REQUIRING_MATH` if it uses
libm). New-benchmark checklist:

1. Prove the checksum fits: show max intermediate and accumulator bounds
   against the declared type (the `array_matmul` n=200 row in P1.3 is the
   worked example of failing this and widening to 63-bit).
2. State the scaling rule that gets the slowest `-O2` config past 50 ms.
3. `file_io`-class benches: stdout is a checksum only (never timing or
   paths); temp files live in `$TMPDIR`/equivalent and are removed on both
   sides even on failure.
4. One paragraph in this plan: what codegen/runtime path it pins and which
   existing benchmark it partners (no orphans — P0.2 is the last orphan
   amnesty).

List (one atomic commit each, bench + baseline + harness entry + plan
paragraph):

- **`fixed_bin63.pli`** — `FIXED BINARY(63)` arithmetic (widening, mixed
  31x63 ops); closes the gap between `scalar_arith` (31-bit) and
  `decimal_ops`. Partners `scalar_arith`.
- **`bcd_decimal.pli`** — `FIXED DECIMAL(25,4)` (more than 18 digits,
  BCD/runtime path), explicitly marked as the DECIMAL-PERF-PLAN.md section 8
  non-goal now being measured. Partners `decimal_ops`; expect `✗`-by-design
  vs `double` baseline with a documented rationale.
- **`string_search.pli`** — `INDEX`/`SUBSTR` scan over a 100 KB buffer
  (exercises runtime string helpers, complements `string_ops`
  concatenation). Partners `string_ops`.
- **`file_io.pli`** — sequential `PUT`/`GET` of 10 000 records through a
  temp file; both sides checksum, stdout is just the checksum so the harness
  match logic works unchanged. Partners `io_throughput`; report throughput
  (MB/s) alongside wall time.
- **`builtin_heavy.pli`** — `ABS`/`MAX`/`MIN`/`MOD`/`DIMENSION`/array
  builtins in a tight loop (pins `typeBuiltin`/`emitBuiltin` codegen
  quality). Partners `math_functions` (non-transcendental side).
- **`deep_recursion.pli`** — mutually recursive procedures plus `AUTOMATIC`
  array frames (partners `proc_calls`, stresses the call path rather than
  leaf arithmetic).
- **`zerodivide_storm.pli`** — carved out of `on_conditions`: 100% faulting
  divisions vs 1% faulting, so handler cost is isolated from branch cost.
  Partners `on_conditions`.
- **`size_storm.pli`** (deferred until P1.6+P1.7 land) — the SIZE analogue:
  forced `--size-checks` with a tripping integer loop. Do not attempt before
  the flags table and the checks-forced job exist.

Non-goal until codegen distinguishes float precisions (see B8): any
short-float benchmark — all `FLOAT` is `double` today, so it would measure
nothing.

Sketch for `fixed_bin63.pli` (template for the rest):

```pli
 test_bin63: PROCEDURE OPTIONS(MAIN);
   DECLARE (i, n) FIXED BINARY(31);
   DECLARE (a, b, sum) FIXED BINARY(63);
   n = 200000;
   a = 1000000000000;
   b = 999999999999;
   sum = 0;
   DO i = 1 TO n;
     sum = sum + a - b + i;
     a = a + 1;
     b = b + 1;
   END;
   PUT SKIP LIST(sum);
 END test_bin63;
```

```c
#include <stdio.h>
#include <stdint.h>

int main(void) {
    int32_t i, n = 200000;
    int64_t a = 1000000000000LL, b = 999999999999LL, sum = 0;
    for (i = 1; i <= n; i++) {
        sum = sum + a - b + i;
        a = a + 1;
        b = b + 1;
    }
    printf("%lld\n", (long long)sum);
    return 0;
}
```

---

## 6. P3 — Methodology (after P0-P1 land)

- Pin runs (`taskset` on Linux, `nice -20` plus single-runner on macOS),
  record machine/LLVM version into each JSON.
- Track ratios in CI as a trend chart, not a gate — fail only on output
  mismatch or worse than 2x regression vs rolling median.
- Refresh the OPTIMIZATION-PLAN.md status table with post-P1.3 numbers and
  close the `condition_ops -O3 0.24x` anomaly with a root-cause note.

---

## 7. Suggested order

P0.1 -> P0.2 -> P0.4 -> P0.5 -> P0.6 (one atomic commit each, trivial) ->
P1.1/P1.5 (harness hygiene, no behavior change) -> P1.6 -> P0.3 (needs P1.6)
-> P1.3 (workload scaling, re-measure everything, refreshes the B4 table) ->
P1.2/P1.4 (statistics + float precision; expect newly exposed `≈` findings)
-> P1.7 (checks-forced job; clear or justify waivers) -> P2 one benchmark
at a time per the checklist. `size_storm` waits for P1.6+P1.7.
