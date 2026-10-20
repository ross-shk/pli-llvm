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

## 3. WS-A: what the inferred memory effects are worth (P1c)

`benchmarks/lang/pure_call.pli` calls a top-level helper in a hot loop. The
paired comparison builds the same program twice — once with the inferred
procedure memory effects (ADR-264) and once with `--no-memory-effects`,
which leaves every call with LLVM's default effects:

| build | median |
|---|---|
| no effects | 69.70 ms |
| effects | 69.41 ms |

**Ratio 1.00x — no measurable difference.** (An isolated run of the same
pair measured 1.01x; the two builds sit inside each other's noise.) The
PL/I-vs-C ratio at `-O2` is 0.41x, because clang keeps the twin's helper out
of line while LLVM specializes the PL/I one.

Nothing else moved: the C-parity corpus' geomean at `-O2` is **0.82x**,
the same as the pre-P1c baseline, so the attributes cost the acceptance gate
nothing.

The reason is worth recording, because it decides what WS-A can be
expected to deliver: **LLVM's inliner already absorbs the helper**. A
promise that "this call writes no memory" only buys something when the
call survives, and the optimizer removes exactly the calls the promise
would have helped. The inference is still the right thing to emit — it is
correct, it is cheap, and it is the only channel by which a PL/I fact can
reach the optimizer — but its value is *latent* until a callee LLVM cannot
inline exists.

Two shapes were tried before settling on the witness, and both are recorded
because they are the shapes a reader would try first:

- A small helper with a loop in it: LLVM inlines it and then folds the
  whole caller's loop into a constant, because after inlining the loop
  becomes affine. The program runs in under 10 ms and measures nothing.
- A helper large enough (300 statements) that LLVM leaves it out of line:
  the call survives, but the caller's loop is then folded away anyway,
  because the sum it computes has a closed form.

So the witness uses the corpus's own trick — an array indexed by a modulo,
which keeps the loop from folding (`checks_on` uses the same shape) — and
accepts a 1.01x result.

**What A4's gate found.** The plan's gate for the alias taxonomy was: "a
loop that writes a local and reads a STATIC each iteration. If LLVM already
keeps them apart, close without a diff and record the result." LLVM already
keeps them apart: `opt -passes=aa-eval` reports **NoAlias** between an
alloca and a global. The positive direction of A4 therefore closes without
a diff; what remains is the taxonomy as one function (ADR-265) and the two
litmus tests that guard it.
