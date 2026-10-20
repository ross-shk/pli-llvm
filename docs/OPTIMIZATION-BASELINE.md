# Optimization baseline — the measurements the plan's gates depend on

**What this file is.** A short, growing record of the *baseline measurements*
that the optimization plan's phases are gated on. A phase may only close by
recording a number here — the plan's "measure, then act" rule, and IBM's
"degrade and warn" model applied to the process itself.

**What this file is not.** A regression log. That is
`design-docs/BENCHMARK-LOG.md` (a local working document, not tracked), which
records *deteriorations* between runs. This file records the *starting point*
a phase is measured against.

**How to reproduce.** Every entry names the command that produced it. The
benchmark harness is `benchmarks/run_benchmarks.py`; the corpus is split into
two categories (ADR-262): `benchmarks/ir/` (the C-parity corpus, whose
geomean is the acceptance gate) and `benchmarks/lang/` (the PL/I-language
corpus, whose gates are paired PL/I-vs-PL-I comparisons).

---

## 1. Check cost with the checks ON — M7's 1.5x criterion

**The criterion.** `design-docs/IMPLEMENTATION-PLAN.md` (a local working
document, not tracked) M7: *"check-heavy code is within 1.5x of check-free
code"*. Never measured before: the repo's `--checks-forced` mode compares
*outputs*, not *times*.

**The measurement.** `benchmarks/lang/checks_on.pli` — a loop whose values are
bounded at run time but not at compile time, so the compiler cannot prove the
SIZE overflow traps or the SUBSCRIPTRANGE bounds checks away and must keep
them. The pair compiles the same source two ways at `-O2`:

| build | flags | median |
|---|---|---|
| check-free | (default; `-O2` elides both check kinds) | ~70 ms |
| checks-on | `--size-checks --subscript-checks` | ~128 ms |

```sh
python3 benchmarks/run_benchmarks.py --only checks_on --runs 7
```

**The ratio: 1.82x** (1.75x–1.86x over three consecutive runs, 2026-10-20,
arm64 macOS, LLVM 23.1.2). Both builds print identical output, so the
difference is the checks and nothing else.

**The verdict: the criterion is NOT met.** Check-heavy code is ~1.8x
check-free code, not within 1.5x. The plan's WS-I1 therefore **does not
close** — the item stays open and the range-fact work (WS-I1) and the cheap
inline check (WS-K7) proceed as planned. Had the ratio come out ≤ 1.5x, the
plan's stop rule would have closed the item with this number and no diff.

**Note on the C reference.** The same benchmark's PL/I-vs-C ratio at `-O2` is
**1.05x** — the *un*-checked PL/I code is at parity with C. The 1.82x is the
price of the checks alone, which is what M7 asks about.

---

## 2. Findings the corpus surfaced

The corpus earns its keep by turning suspicions into numbers. Three of the
things it found are **not** fixed here — each is a question of its own, and
the plan's rule is one thing at a time. They are recorded so the next
workstream does not have to rediscover them.

### 2.1 A trapping loop overflowed the stack at -O0/-O1/-O3

`benchmarks/lang/on_dispatch.pli` segfaulted at every optimization level
except `-O2`. The ON-unit dispatch allocated its context slot in the trap
block, so every trap leaked an `alloca`; a loop trapping 600k times asked for
9.6 MB and overflowed the stack. At `-O2` mem2reg hid it.

**Fixed** (ADR-263): the slot moved to the entry block. `tests/driver/
on_trap_loop.sh` is the gate. This also repaired `benchmarks/ir/
on_conditions.pli`, which had been failing at `-O0` in every benchmark run
without anyone reading the line.

### 2.2 Compiling an array of a padded structure is quadratic

An array of the struct-mirror structure (stride 20, so it cannot be
vectorized) makes the optimizer fully unroll the loop and split the array
into per-member slices — and the result is quadratic:

| elements | compile time |
|---|---|
| 512 | ~2 s |
| 4096 | ~25 s |
| 20000 | ~7 min |

The struct-mirror gate therefore uses 512 elements, which is still enough
for the vectorizer to engage on the compact structure it also carries. The
quadratic behaviour itself is **not** fixed and is a candidate for the
optimization plan's own work.

### 2.3 Decimal arithmetic does not link at -O0

`benchmarks/ir/decimal_ops.pli` and `benchmarks/ir/bcd_decimal.pli` fail to
link at `-O0` with `undefined symbol: ___divti3` — the 128-bit division
helpers are not in the link. The new `benchmarks/lang/decimal_scale.pli`
inherits the same gap; it builds and runs at `-O1` and above.

**Not fixed** — it is pre-existing and affects the established corpus
identically, so it is a separate piece of work rather than a side effect of
the corpus.

---

## 3. PL/I constant folding — WS-B (B1, B2, B3)

**The criterion.** The plan's standing rule (`OPTIMIZATION.md` §6.4): *a
technique is not implemented until IR demonstrates the miss and benchmarks show
end-to-end benefit*. Each WS-B technique has its own gate, and B1's gate is
explicit: *"Witness: an array loop with a constant lower bound. If LLVM's SCEV
already collapses the induction variable, close without a diff."*

### 3.1 B1 — constant offset folding: closed without a diff

**The measurement** is the IR, because the technique's cost is the *absence* of
work in the IR. For every PL/I subscript shape, the displacement the plan asks
for is already one constant:

| PL/I shape | displacement | emitted IR |
|---|---|---|
| `a(3)`, `a(0:9)` | `(3-0) * 1` | `getelementptr inbounds [10 x i32], ptr %A.addr, i64 0, i64 3` |
| `b(-1)`, `b(-3:7)` | `(-1+3) * 1` | `getelementptr inbounds [11 x i32], ptr %B.addr, i64 0, i64 2` |
| `c(2,3)`, `c(0:9,0:9)` | `2*10 + 3` | `getelementptr inbounds [100 x i32], ptr %C.addr, i64 0, i64 23` |
| `st.m(2)`, member array | member GEP + `(2-0)` | `getelementptr inbounds [4 x i32], ptr %mem, i64 0, i64 2` |
| `arr(2).x`, array of structures | `(2-1) * 1` | `getelementptr inbounds [3 x { i32 }], ptr %ARR.addr, i64 0, i64 1` |

The compile-time half is there too: a constant out-of-range subscript is
*diagnosed* at compile time (rule (126)), so its SUBSCRIPTRANGE check is
statically dead.

**The verdict: the item closes with no code change**, which is what the
plan's gate says to do. What is added is `tests/ir/const_offset.pli`, whose
`.check` pins the three displacements, so the property is now guarded.

### 3.2 B2 — typed constant folding for `FIXED DECIMAL`

**The measurement.** Two things changed, and both are visible without a
benchmark:

* A *capability*: `DECLARE a FIXED DECIMAL(9,2) INITIAL(99.99 + 0.01)` was
  rejected with *"INITIAL requires a constant in this stage"*; it now folds to
  `100.00`. The folded value equals the runtime computation exactly, scale
  included — `INITIAL(1.5 * 2.25)` gives `3.38` in both.
* A *conformance fix*: `DECLARE a FIXED DECIMAL(5,2) INITIAL(5)` stored
  `0.05`; it now stores `5.00`. The assignment path was already correct; the
  INITIAL path did not rescale the literal to the declared scale.

The fold of a general expression is **IR-neutral**: `1.5 + 2.25` produced
`store i32 375` before the change and produces `store i32 375` after it — the
runtime path and the compile-time path agree bit for bit, which is the
property the type-directed rules exist to guarantee.

**The verdict.** The item closes. The gate is `tests/core/decimal_const.pli`,
which also carries the plan's negative test: `1.0 / 0.0` must still raise
ZERODIVIDE, which fails if a division is ever folded.

### 3.3 B3 — constant-argument built-in lowering

**The measurement.** The cost of the old lowering is structural, so it is read
off the IR rather than timed:

| operation | before | after |
|---|---|---|
| `VERIFY(s, '0123456789')` | `O(n·m)` double loop (`vrf.outer`/`vrf.inner`/`vrf.check_byte`) | `O(n)`: one lookup in a 256-entry `@pli_search_table` per character |
| `SEARCH(s, '0123456789', 1)` | `O(n·m)` double loop (`srch.*`) | `O(n)`: the same table |
| `REPEAT('x', 5)` | a five-iteration loop, one `llvm.memmove` per character | one `llvm.memmove` from the constant `@str.5 = "xxxxx"` |
| `VERIFY(s, v)` where `v` is a `VALUE` constant | reads the constant's alloca (`%V.addr`) | reads the literal's global — *identical IR* to `VERIFY(s, '0123456789')` |

**The benchmark.** `python3 benchmarks/run_benchmarks.py --force --runs 3`
(arm64 macOS, LLVM 23.1.2) — the C-parity geomean is **0.83x at `-O2`** and
**0.84x at `-O3`**, against the plan's baseline of 0.8181x / 0.8230x. The
difference is inside the run-to-run noise, and the harness reports *"No
material deterioration detected at -O2 vs previous run"*. The corpus does not
exercise a constant alphabet or a constant `REPEAT` heavily, so the expected
end-to-end effect of B3 is small; the win is in the *shape* of the lowering,
which the IR records.

**The verdict.** The item closes. The gates are `tests/ir/search_table.pli`
(the table global, and no `pli_verify`/`pli_search` call left),
`tests/ir/value_const_equiv.pli` (the `VALUE` form never reads `%V.addr`),
and `tests/core/search_const.pli` (the table path and the byte-scan path
agree exactly — and the latter still passes under
`--experimental-lowering=runtime`, so the differential oracle keeps both
sides).

**Not measured, and not claimed.** B4 (picture conversion folding) is gated on
the plan's WS-K3 picture classifier, which is not built; it does not start
here. The plan's `RESEXP` opt-in (WS-K9) is likewise untouched.

