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

## 3. PL/I constant folding — WS-B (B1, B2, B3, B4)

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
`.check` pins the three displacements, so the property is now guarded, and
`tests/core/lower_bound.pli`, the plan's own witness program — `A(-3:7)`,
`B(0:9)` and `C(1:10)` over the same sequence — which requires the three
spellings to agree value for value.

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
ZERODIVIDE, which fails if a division is ever folded. Its positive cases
compute the same expression twice, once from literals and once from variables,
and require the two results to be equal — so the test proves the fold agrees
with the runtime, not that a constant equals itself.

**The plan's second negative test has no witness.** A fold whose result would
not fit the declared `PRECISION` is refused, but nothing in the repository can
reach that refusal: every decimal result type the front end computes is wide
enough by construction (`arithResultType` gives 31 digits of storage, and
`mulResultType` widens the product to 128 bits when the operand precisions add
up past 18), and a declared precision is not enforced on assignment anywhere —
a pre-existing gap, unchanged here. The guard stays as a safety net; the
observable negative test is ZERODIVIDE.

### 3.3 B3 — constant-argument built-in lowering

**The measurement.** The cost of the old lowering is structural, so it is read
off the IR rather than timed:

| operation | before | after |
|---|---|---|
| `VERIFY(s, '0123456789')` | `O(n·m)` double loop (`vrf.outer`/`vrf.inner`/`vrf.check_byte`) | `O(n)`: one lookup in a 256-entry `@pli_search_table` per character |
| `SEARCH(s, '0123456789', 1)` | `O(n·m)` double loop (`srch.*`) | `O(n)`: the same table |
| `REPEAT('x', 5)` | a five-iteration loop, one `llvm.memmove` per character | one `llvm.memmove` from the constant `@str.5 = "xxxxx"` |
| `VERIFY(s, v)` where `v` is a `VALUE` constant | reads the constant's alloca (`%V.addr`) | reads the literal's global — *identical IR* to `VERIFY(s, '0123456789')` |
| `TRANSLATE(s, 'XY', 'abc')` | `O(n·m)` double loop (`tr.inptr`/`tr.kptr`) | `O(n)`: one lookup in a 256-entry `@pli_translate_table` per character |

**The differential oracle found two defects.** `TRANSLATE('abcxyz', 'XY',
'abc')` printed `XYcxyz` under `--experimental-lowering=runtime` and `XY xyz`
inline: the runtime let a match past the end of `out` pass through, where PL/I
blanks it. Fixing that exposed a second, worse one — the result temporary is a
plain `alloca`, and neither lowering wrote the field past the copied text, so a
short source left stack residue in a fixed-length result. Both are fixed
(ADR-264); the oracle is what made them visible, which is the argument for
keeping it a *true* differential rather than a second copy of the same code.

**The benchmark.** `python3 benchmarks/run_benchmarks.py --force --runs 3`
(arm64 macOS, LLVM 23.1.2, after a full rebuild) — the C-parity geomean is
**0.81x at `-O2`** and **0.83x at `-O3`**, against the plan's baseline of
0.8181x / 0.8230x. The movement from the previous run (0.83x / 0.84x) is inside
the run-to-run noise, and the harness reports *"No material deterioration
detected at -O2 vs previous run"*. No benchmark in the corpus calls `TRANSLATE`
and none leans on a constant alphabet or a constant `REPEAT`, so the expected
end-to-end effect of B3 is small; the win is in the *shape* of the lowering,
which the IR records.

**The verdict.** The item closes. The gates are `tests/ir/search_table.pli`
(the table global, and no `pli_verify`/`pli_search` call left),
`tests/ir/value_const_equiv.pli` (the `VALUE` form never reads `%V.addr`),
`tests/core/search_const.pli` (the table path and the byte-scan path
agree exactly — and the latter still passes under
`--experimental-lowering=runtime`, so the differential oracle keeps both
sides), and `tests/ir/translate_table.pli` + `tests/core/translate_const.pli`
for `TRANSLATE` (ADR-264).

### 3.4 B4 — picture conversion folding

**The gate.** B4 was blocked on the plan's WS-K3 picture classifier, and that
classifier did not exist. It now does: `pli::classifyPicture()` in
`src/irgen_decisions.cpp` validates a picture (rules (146)-(148)), expands
repetition factors, separates a trailing `F(±n)`, and records the width, the
assumed point and the integer/fractional digit positions. `foldable` marks the
numeric subset the constant folder can reproduce with the runtime's exact
output: `9 Z V . + - S X A`, no repetition, no scaling factor.

**The IR.** The folded item no longer reaches the interpreter:

| item / picture | before | after |
|---|---|---|
| constant `123.45`, `P('ZZ9.99')` | `call @pli_put_edit_pic(i64 12345, ...)` | `call @pli_put_edit_char(ptr @str, i64 6, i64 6)` with the literal text `123.45` |
| variable `v`, `P('ZZ9.99')` | `call @pli_put_edit_pic(...)` | unchanged |
| constant item, non-foldable `P('ZZ9,99')` | `call @pli_put_edit_pic(...)` | unchanged |

`tests/ir/picture_fold.pli` pins all three rows, so the fold is guarded on
every target and the fallback cannot silently disappear.

**The measurement.** `benchmarks/lang/picture_fold.pli` performs 1,000,000
conversions of a constant value through `'ZZ9.99'` into a `STRING` buffer and
checksums the first byte. The pair compiles the same source twice at `-O2`,
once on the folded path and once with
`--experimental-lowering=picture:runtime`; the same two lowerings were built at
`-O0` for the table. Median of nine interleaved direct runs (arm64 macOS,
LLVM 23.1.2):

| build | `-O0` | `-O2` |
|---|---|---|
| folded | 24.6 ms | 17.6 ms |
| runtime interpreter | 112.4 ms | 16.3 ms |
| ratio (runtime / folded) | **4.6x** | 0.93x (noise) |

At `-O0` the fold removes the interpreter and the benefit is unambiguous. At
`-O2` there is nothing left to win: `pli_put_edit_pic` is `alwaysinline`, its
arguments are constants, and LLVM folds the inlined formatter itself. The
nine-run harness pair at `-O2` reports 0.95x; an earlier five-run pair
reported 0.92x — both inside the noise floor — so **no `-O2` speedup is
claimed**. The durable result is the IR shape: the conversion is gone *before*
the optimizer runs instead of depending on an inliner to remove it, and the
unoptimized path gets the 4.6x.

**The verdict.** B4 closes. The gates are `tests/core/picture_fold.pli`
(folded literal vs runtime variable, covering signs, zero, `X`, `V`, float
truncation and a non-foldable fallback), `tests/ir/picture_fold.pli` (the
three rows above), the picture witness in
`tests/driver/experimental_lowering.sh` (identical output under `auto`,
`runtime` and `llvm`), and the benchmark pair above. Repetition factors and
`F(±n)` are classified but deliberately stay on the runtime path for folding;
the full picture grammar, `PICTURE` variables and `GET EDIT P` remain D1.

**Still not measured, and not claimed.** The plan's `RESEXP` opt-in (WS-K9) is
untouched. The B3 and B4 gates also ask for the measurement on both targets;
no x86 host was available, so the numbers above are arm64 only. Both changes
are target-independent by construction — a constant global and an indexed
load, no new intrinsics, no ABI change — and the IR tests pin the shape on
every platform CI builds.


---

## 4. WS-C — the IRGen peepholes (C1, C3, C4, C5)

**The criterion.** The plan's standing rule applies to every item: a technique
is not implemented until the IR shows the miss and a benchmark shows the gain.
WS-C is *secondary* to the decision layer (WS-K) after the IBM-precedence
reframe (§1.6 of the plan): a peephole is attempted only when the decision
layer cannot express the transformation. That left four items — C1, C3, C4 and
C5 — and each is recorded here with its own verdict.

### 4.0 The differential check both peepholes share

`tests/driver/peephole_fuzz.sh` generates random programs — concatenations of
fixed-length variables and literals (empty literals and nested concatenations
included, so the result is truncated and blank-padded) and masked bit tests on
one value (disjoint 4-bit fields, which merge, and overlapping masks, which
must not) — and compiles each one twice: once with the peepholes, once with
`--experimental-lowering=concat_const:runtime --experimental-lowering=bit_merge:runtime`.
The two runs must print the same thing. The seed is fixed, so the corpus is the
same on every run, and the test takes about twenty seconds.

The committed corpus is eight programs. A wider run of the same generator over
790 random programs (390 during the review and 400 in the background) found
**no divergence**, with 2554+ constant-offset concat stores and 594+ merged
bit tests exercised — so the comparison was never vacuous.

### 4.1 C1 — constant-offset concatenation stores: **implemented**

**The measurement** is the IR first, because the cost of the old lowering is
structural. `A = B || C` with a fixed-length `A(n)` and fixed-length `B`, `C`
produced:

| step | before | after |
|---|---|---|
| the result buffer | `call ptr @pli_alloc(i64 7)` — a heap allocation per evaluation | *gone* |
| the leaf copies | `memcpy` with a runtime length, at a runtime offset | one `memcpy` of a constant size at a constant offset |
| the assignment | `memmove` + `memset` with runtime lengths | *gone* — the leaves land in the target directly |
| the buffer | `call void @pli_free(ptr %cbuf)` | *gone* |

`tests/ir/concat_const.pli` pins that shape: two `llvm.memcpy` calls of
constant size, the second at `getelementptr i8, ptr %C.addr, i64 10`, and no
`pli_alloc` anywhere in the procedure.

**The benchmark.** `benchmarks/lang/concat_const.pli` — two `CHAR(10)` values
concatenated into a `CHAR(20)` target, four million times.
`python3 benchmarks/run_benchmarks.py --only concat_const --runs 5 --force`
(arm64 macOS, LLVM 23.1.2, after a rebuild); median of five:

| build | PL/I | C twin | ratio |
|---|---|---|---|
| `-O0` before | 153.4 ms | 21.9 ms | 7.01x |
| `-O0` after | 46.2 ms | 22.5 ms | 2.06x |
| `-O1` before | 121.6 ms | 8.6 ms | 14.12x |
| `-O1` after | 7.8 ms | 9.0 ms | 0.87x |
| `-O2` before | 117.1 ms | 22.2 ms | 5.28x |
| `-O2` after | **7.5 ms** | 8.8 ms | **0.85x** |
| `-O3` before | 119.4 ms | 8.7 ms | 13.70x |
| `-O3` after | 7.9 ms | 9.9 ms | 0.80x |

The `-O2` build is **15.7x** faster and now beats its C twin. The win is the
allocation, not the copy: the old path paid a `malloc`/`free` pair per
iteration to move twenty bytes whose offsets were known at compile time.

**The verdict.** The item closes, with ADR-242. The correctness gates are
`tests/core/concat_pad.pli`: the value the store must produce (blank padding
to the target's length, truncation to the leftmost characters, literal and
empty leaves) and the two shapes the store *declines* — the target as a leaf
and the target as the second leaf — where the concatenation must be read whole
before anything is written. Those two cases are the aliasing hazard the
transformation introduces, and they keep the generic path.

### 4.2 C3 — merge bit tests within one storage object: **implemented, kept**

**The measurement** is the IR, and the benchmark. `benchmarks/lang/bit_merge.pli`
tests two masked fields of one 32-bit word in a single conjunction, 300
million times; the C twin tests the same two fields.

The merge is `(X & M1) = E1 & (X & M2) = E2` → `(X & (M1|M2)) = (E1|E2)`, which
is valid exactly when the masks are disjoint and each expectation lies inside
its own mask. The rule is `pli::mergeBitTests` in `src/irgen_decisions.cpp` —
a decision, so it lives outside `irgen.cpp` (the plan's C2). The IR loses one
load, one mask, one pack and one comparison.

The benchmark (arm64 macOS, LLVM 23.1.2):

| build | merged | separate | ratio |
|---|---|---|---|
| `-O0` | 2.63 s | 3.64 s | **1.38x** |
| `-O2` (pair, 7 runs) | 70.2 ms | 70.4 ms | 1.00x |

**The finding is the interesting part: at `-O2` LLVM already does this merge.**
The two builds' optimized IR is the same code — `and <4 x i32> %x, splat (i32
13056)` and `icmp eq <4 x i32> %x, splat (i32 13056)`, once per vectorized lane
— because LLVM's `instcombine` rewrites `(x & a) == a && (x & b) == b` with
disjoint `a` and `b` into the merged test on its own.

**The verdict.** The plan's gate says the decision is recorded either way: the
`-O2` win is under 1%. **The code is kept** — the `-O0` build is 1.38x faster
with it, and the merge is in the IR before the optimizer runs, so it holds at
every level and for every shape LLVM's heuristics miss. This is the same
argument that kept ADR-266's picture fold, which also showed no `-O2` gain.

The gates are `tests/ir/combine_tests.pli` (one `icmp eq i32` against the
merged expectation, and no second comparison), `tests/core/bit_merge.pli` (the
merged conjunction equals the two tests taken separately over eight values, and
the two shapes the merge must decline — overlapping masks, and two different
values — keep their own value), and the witness in
`tests/driver/experimental_lowering.sh` (the merged and the separate lowering
agree in the `auto`, `runtime` and `llvm` modes).

### 4.3 C4 — the `optimize_if` contract: **closed as an invariant**

**The transformation is not ported.** MULTICS branched on the comparison flags
and never materialized the 0/1; LLVM has no flag-only branch, and
`InstCombine` folds the `icmp` into the branch anyway. The plan says so
("Do not port the transformation itself").

**The contract is recorded.** MULTICS's `optimize_if.pl1:215-218` sets
`save_temps` on a label the optimizer did not know about: a transformation that
synthesizes a new basic block or a new join must re-materialize the values it
cached across that edge. ADR-269 states that as an invariant of the code
generator, and the comment at the one place where a cached pointer does cross a
synthesized join — the CONTROLLED generation expansion in `storeTo`, whose
`mergeBB` join may have a new address on one arm — now says so. The other
cached value, the CONTROLLED address hoist in a DO loop, is sound because the
analysis proved the generation cannot change inside the loop; that comment
names the obligation too.

No behaviour changed, so there is nothing to benchmark: the item's cost was a
constraint to be written down, and the constraint is now written down.

### 4.4 C5 — the exponent patch for a scaled `FIXED BINARY`: **deferred**

**The gate.** The plan's C5 says: *"If the repo does not support binary scale
at all, **defer** — check `Type` for a binary-scale field first."* The field
exists — `Type::scale` carries it for `FIXED BINARY(p,q)` — but nothing uses
it:

| where | what a binary scale would need | what the compiler does |
|---|---|---|
| a literal assigned to a scaled `FIXED BINARY` | the literal scaled by 2^q | stored unscaled (only `FIXED DECIMAL` is rescaled, ADR-240) |
| `+` between two different binary scales | the operands rescaled to a common scale | the raw integers added (sema takes the larger scale, irgen does not rescale) |
| conversion to `FLOAT` | the value divided by 2^q | the raw integer converted (`sitofp` only) |
| list-directed output | the 2^q point printed | the integer printed |

`docs/GRAMMAR-COVERAGE.md` (16),(17) says it plainly: *"`FIXED BINARY(p,q)`
stays 2-based (unscaled)"*, and `tests/core/scaled.pli` pins that behaviour: a
scale-2 variable assigned the literal `5` holds `5` (not `20`), two scale-2
values multiply to their raw product, and a scaled value prints as an integer.
So the compiler's own test suite asserts the unscaled model — the exponent
patch would contradict it.

**The verdict.** The exponent patch cannot be ported on its own: multiplying
by 2^-q in the float conversion alone would disagree with every other path
that treats a binary-scaled value as unscaled. The prerequisite is the
*language feature* — `FIXED BINARY(p,q)` semantics — which is rules (16),(17)
work, not an IRGen peephole, and which would need its own ADR and its own
rule-coverage rows. The plan's gate therefore applies: **C5 is deferred**, no
ADR-244 is written, and ADR-244 stays reserved for it. When the language
feature lands, the conversion becomes an exponent patch — `sitofp` followed by
a multiply by 2^-q, exact over the whole range — and the decision is already
recorded here.

### 4.5 C7 — "test vs. move": **closed, already covered by LLVM**

**The claim.** MULTICS had two machine forms for `VERIFY`/`SEARCH`: a cheap
*test* that only sets the comparison flags, and a full *move* that produces the
value. `compile_exp.pl1:494-496` picks the test form when the value is not
needed:

```pl1
/* we can't use the "for_test" code sequences if result is needed elsewhere */
		cg_stat$for_test_called = cg_stat$for_test_called & ref1 -> reference.ref_count <= 1;
```

LLVM has no flag-only form, but the idea survives as *the value is compared and
the comparison is the branch*, and LLVM folds the value away when it is dead.

**The verification.** `tests/ir/test_vs_move.pli` pins the two shapes in the
compiler's own IR:

| shape | the compiler's IR |
|---|---|
| the value is used only in a condition | `%tst = icmp ne i32 %ver32, 0` → `br i1 %tst` |
| the value is needed elsewhere | `store i32 %srch32, ptr %X.addr` → the comparison reads the stored value |

Optimizing that module (`clang -O2 -S -emit-llvm` over the compiler's
`-emit-llvm` output, the same method ADR-243's finding used) shows what LLVM
does with the two shapes:

```llvm
%vrf.si.pr = load i8, ptr %S.addr, align 1
%.not  = icmp eq i8 %vrf.si.pr, 97      ; the verify: one comparison, no value
%.mux  = zext i1 %.not to i64           ; the search: the value is needed, so it stays
```

The verify's *position* value is never materialized — the comparison **is** the
test, which is MULTICS's flag-only form — while the search's value survives
because something else needs it. That is MULTICS's `ref_count <= 1` guard,
expressed as LLVM's SSA use count: LLVM removes the materialization exactly
when the value has no other use.

**The verdict.** Closed without a diff: the transformation is LLVM's, the
guard is LLVM's, and the IR check pins the shape both rely on.

### 4.6 C8 — the infinite loop's condition and the `NOT` of a `BIT(1)`: **closed, one case deferred**

**The loop case.** The plan asks for "`CHECK-NOT` for a dead condition". The
compiler does not even emit one: a constant `DO WHILE` condition is folded
*before* the optimizer runs, so the loop's condition block is a branch on the
constant with no comparison in it.

```llvm
do.cond.0:                                        ; preds = %if.end.1, %entry
  br i1 true, label %do.body.0, label %do.end.0
```

`tests/ir/loop_const_cond.pli` pins that (`CHECK-NOT: icmp` between the
condition block and the body). MULTICS's `jump_op.pl1:383-405` did the same
thing by hand; here LLVM's `SimplifyCFG` would have done it anyway.

**The `BIT(1)` case.** MULTICS's `jump_op.pl1:350-381` turns `IF ^ BIT(1)` into
a complemented branch, but only when the value is a single, unshared bit — the
guard is about PL/I's `BIT` padding, because the `NOT` of a padded value is
not the machine's complement. The plan defers this to WS-D1 (the padding
invariant) and asks first whether the compiler's `BIT` representation has
padding semantics at all.

It does not, for `BIT(1)`: the representation is an `i1` (`src/types.h` and the
`loadSym` lowering), so the `NOT` is `xor i1 %b1, true` — the complement of the
one bit, with no padding to get wrong. The program's value is right (`^'1'b` is
false, `^'0'b` is true), and LLVM folds the `xor` into the branch:

```llvm
%not = xor i1 %b1, true
br i1 %not, label %if.then.0, label %if.end.0
```

**The deferral.** For `BIT(n>1)` the `NOT` is byte-wise over the whole array,
so the *padding* bits are complemented too. Nothing observes them today — every
comparison of a `BIT` value drops the padding bits (`packBitValue` shifts them
out) — but the model is not written down yet, so the item stays **deferred to
WS-D1** as the plan says. The deferral costs nothing today; it is a note for
the day WS-D1 models the padding.

### 4.7 C9 — reference-count-driven temp elision: **closed, already covered by LLVM**

**The claim.** MULTICS's `need_temp.pl1` and `state_man.pl1:1330-1354
need_ref` walk the expression tree to decide whether a temporary is needed,
and the `ref_count` of a reference decides whether a node's value is needed
elsewhere.

**The modern equivalent.** In LLVM a value's uses are its `ref_count`: an SSA
value with one use is dead after that use, and `InstCombine`/`DSE` remove the
computation that produced it. The repo's IR is SSA from the start, so the
analogue is exact.

**The verification.** The compiler's own IR shows the modern form of MULTICS's
guard: a value that is used once is compared and the comparison is the branch
(the `verify` case of §4.5), and a value that is used twice is materialized
once and read back (the `search` case). LLVM then does exactly what MULTICS's
`need_ref` did — remove the temporary whose value nobody needs.

**The verdict.** Closed without a diff, and no IR check: the property is a
property of SSA, not of this compiler's lowering. Nothing to guard, because
nothing in the compiler decides whether a temporary is needed — LLVM does.
