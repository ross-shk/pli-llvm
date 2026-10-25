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

**How the sections are ordered.** By the phase that closed, not by the
workstream's number: §3-§7 are the memory-effect, string, ON-dispatch,
aggregate and I/O phases, and §8-§11 are the constant-folding, peephole,
decimal and check-cost phases, which came in with the folding branch. The
plan's own workstream numbering is the plan's, not this file's.

---

## 1. Check cost with the checks ON — M7's 1.5x criterion

**The criterion.** The planning document this baseline was written against is not
tracked; its remaining rows now live in `DEFERRED-WORK.md`. Its M7 gate:
*"check-heavy code is within 1.5x of check-free code"*. Never measured before:
the repo's `--checks-forced` mode compares *outputs*, not *times*.

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
| no effects | 60.78 ms |
| effects | 60.87 ms |

**Ratio 1.00x — no measurable difference.** Three isolated runs of the pair
gave 1.00x, 1.00x and 1.00x (60.78/60.87 ms, 60.03/60.01 ms,
59.98/59.95 ms). The PL/I-vs-C ratio at `-O2` is 0.41x, because clang keeps
the twin's helper out of line while LLVM specializes the PL/I one.

Nothing else moved: the C-parity corpus' geomean at `-O2` is **0.78x**, and
0.80x on a second full run, against the 0.82x pre-P1c baseline — the same
figure within the noise of a shared host. The warning the runner prints
("previous run used compiler …") is about that noise, not a change: the
regression check found no deterioration at `-O2`.

**Why the witness shows nothing, precisely.** It is not only that the
inliner absorbs the helper. LLVM infers the memory effects of *any* callee
whose body it can see, from the body itself — so for a same-module callee
the compiler's own inference is redundant, however good it is. The
inference is worth something only for a callee whose body the optimizer
cannot see: **a procedure in another compilation unit**, whose only
description to the caller is the set of attributes on its definition. That
is the shape MULTICS and IBM worked in, and it is the shape the next
measurement should use. In a single-module program the attributes are a
correctness statement and a diagnostic, not a speedup.

To make that claim testable the inference now also proves `nounwind` and
`willreturn` (ADR-264): the three together are what let LLVM speculate a
call and hoist it out of a loop. A driver-level check of the hoisting
itself is the natural next gate, and a two-unit benchmark — one unit
calling a procedure the other defines — is the natural next measurement.

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

**What A3's gate found.** The plan's gate for subobject identity was: "a
program that reads `SUBSTR(S,1,3)` and `S(1:3)` of the same string in one
expression. If LLVM already CSEs them, close without a diff." The second
spelling does not exist in this compiler: a substring reference `S(1:3)` is
not in TR 25.084 (the document has no substring syntax at all) and the parser
diagnoses it — `expected ')', found ':'` — so the two spellings can never
coexist. The reachable witness is the same builtin twice, and LLVM already
folds it: `opt -passes=default<O2>` over the emitted module reduces
`substr(s,1,3) = substr(s,1,3)` to the constant `1`, with no `memmove` and
no comparison loop left. A3 therefore closes without a diff: the identity the
plan wanted to give the PL/I subobject is already the identity LLVM gives the
load, and the one spelling that would have needed a canonical form is not in
the language.

### 3.1 The two-unit measurement: what the promise is worth when it cannot travel

`benchmarks/lang/pure_call_split.pli` is `pure_call.pli` with the helper moved
into its own compilation unit (`pure_call_lib.pli`); the caller declares it as
an external entry (rule 42). The caller's optimizer never sees the callee's
body, so it cannot infer the callee's effects either — the only description
of the callee it has is its declaration.

| build | median |
|---|---|
| split, no effects | 186.77 ms |
| split, effects | 186.03 ms |
| same unit (the callee visible) | 60.86 ms |

**Ratio 1.00x for the inference across the unit boundary; 0.33x for the split
itself.** Two independent runs gave 1.00x and 1.01x for the first pair, and
0.32x and 0.33x for the second; both builds of both pairs print the same
value.

The reading is precise: **the promise does not travel.** The attributes are on
the *definition*, in another object file; the caller's declaration carries
none, so the call keeps LLVM's default effects and cannot be hoisted out of
the loop. The third row is the size of the prize: the same program with the
callee in the caller's own unit runs three times faster, because there the
optimizer sees the body — and infers the very same facts by itself, which is
why the same-unit pair measures 1.00x.

So A1's inference is a correctness statement and a diagnostic in every shape
measured so far, and a speedup only where the promise is delivered to a caller
that cannot see the body. That delivery now exists (ADR-267): the module
that defines the callee publishes what it promises (`--emit-effects=FILE`),
and the module that calls it consumes the publication (`--effects=FILE`) and
attaches the promise to its declaration.

**What the published promise is worth.** The third pair of the same table,
with the publication in place:

| build | median |
|---|---|
| split, no promise | 189.46 ms |
| split, the promise published and consumed | 62.74 ms |
| same unit (the callee visible) | 62.09 ms |

**Ratio 0.33x — the promise captures the whole prize.** The caller cannot see
the callee's body, yet with the published promise its optimizer hoists the
call out of the loop and the program runs at the same speed as the same-unit
build. The optimized IR shows the difference directly: without the promise
the call sits inside the hot loop, with it the call is hoisted into the
preheader and the 150 million calls become one.

So the three measurements together say the whole story: the inference is
worth nothing where LLVM can see the body (it infers the same facts), it is
worth nothing where the body is hidden (the promise does not travel on its
own), and it is worth **3x** once there is a channel that carries it. That
channel is a file the defining module writes and the calling module reads —
the modern form of the module interface PL/I already had.

The LTO leg of the measurement — what the caller gets when the optimizer can
see the body — is now measurable on this host, because the build no longer
mixes two LLVM instances (ADR-268): the in-process LTO link used to crash
there. It measures the same as the published promise:

| build | user time |
|---|---|
| split, nothing | ~0.19 s |
| split, the promise published and consumed | ~0.05 s |
| split, whole-program LTO (the body visible) | ~0.05 s |

**The promise is worth exactly what whole-program visibility is worth** —
about 3x on this witness — and it does not need LTO, a shared build, or the
body: a file the defining module writes and the calling module reads is
enough. That is the whole of WS-A1's value story, and it took a channel to
realise it.


### 2.4 The suite's driver budget is wall-clock, and this host is shared

Twice during the P1c work, `ctest` reported four driver tests as timed out
after 60 s — `multic`, `multimod`, `package`, `struct_mirror` — while the
same suite run directly passed 613 of 613. The four are the same every time,
and they are the heaviest drivers: each links a program and runs it.

The cause is not the compiler. This machine is shared, and a second copy of
the project (`~/Development/Parallelized/PLI/pli-llvm`) was running its own
test suite at the same moment; the load average reached **156 on 8 cores**.
The runner's driver budget is wall-clock, so a test that is merely slow on
a saturated host exceeds it. Reproduced deliberately: with the load above
100 the same four fail, and with the load below 40 they pass — nine
consecutive `ctest` runs, all green.

**Fixed** in `tests/run_tests.py`: the driver budget now scales with the
host's load average (`driver_timeout()`), sampled once at start-up. On an
idle host it stays at 60 s, so a genuine hang is still caught quickly; on a
busy one it grows with the contention, up to 300 s. The exec tests keep
their tight 10 s budget, which a single binary never needs.

A lesson worth keeping: a timeout that is a *symptom* of the host rather
than of the code is worth distinguishing from a real failure, and the
distinction is cheap to make — the same four tests, the same 60 s, and two
completely different verdicts depending on the load.

---

## 4. WS-D: PL/I string semantics (P2)

### 4.1 D1 — the padding invariant: the audit found no bug

The gate asked for an audit before the item was proposed: *write a test that
stores a `CHAR(3)`, reads it back as a wider load, and checks*.

The answer is that the repository cannot produce the bug the gate was
looking for. The representation is `[n x i8]` for a `CHARACTER(n)` and
`ceil(n/8)` bytes for a `BIT(n)`, so there is no storage padding for a load
to run into; a string is fetched as an address plus a length (`IRGen::loadSym`
never loads one wider than the value); and every store is bounded by the
declared extent (`IRGen::storeCharTo` copies `min(dstLen, srcLen)` bytes and
fills `dstLen - n` bytes of tail). A BIT substring assignment — the one
operation that could disturb a neighbour — is not implemented at all: it is
diagnosed ("SUBSTR first argument must be a character string", rule (123)),
not silently miscompiled.

So D1 closes as the plan predicted: a documented invariant (ADR-245) and
regression tests, not a fix.

**How to reproduce.** `tests/core/string_padding.pli` (the values) and
`tests/ir/string_extent.pli` (the IR: `[3 x i8]`, a three-byte copy, a
zero-byte tail fill, a fetch that carries the length three).

### 4.2 D3 — the lifetime markers: a stack-size win, and where it does not reach

The plan's witness was "a loop that builds a VARYING string each iteration".
It does not apply as written: a VARYING *variable* keeps its data inside its
own alloca (`{i32, [n x i8]}`), so there is no separate buffer to mark and
the slot is already reused — the loop writes the same storage every
iteration.

The buffer that does exist is the one a `CHAR(*) VARYING` parameter needs
when the actual is not a VARYING value: a stack copy of the string. Marking
it, together with the by-reference argument dummy and the character
expression buffer, lets the temporaries of different calls share one slot.

**The measurement** — three procedures called with a character expression, in
a loop; the frame size of the emitted IR after `llc -O2`:

| build | frame |
|---|---|
| without the markers | 112 bytes |
| with the markers (ADR-246) | **80 bytes** |

The PL/I-language benchmark corpus does not move: none of its programs passes
a character expression to a procedure, so there is no dummy to share. The win
is a stack-size win on programs that do, not a speed win — the slot was
allocated once either way.

**How to reproduce.** `benchmarks`-independent: compile a program with
several character-expression arguments, emit the IR, and compare the frame
size in the assembly with and without the `llvm.lifetime` calls.

**The safety check.** A wrongly ended lifetime would let a buffer be reused
while it is still live. The VARYING-string corpus and the task programs were
run under AddressSanitizer — eleven string programs and four task programs,
all clean — and `tests/driver/lifetime_asan.sh` keeps that a per-commit
gate. The sanitizer runtime has to match the object, so the test uses the
clang of the compiler's own LLVM and skips when it cannot find one.

---

## 5. WS-F: ON dispatch (P3)

### 5.1 The static handler resolution (Tier 3) is closed by measurement

The `.kilo` plan's Tier 3 — static handler resolution — deferred itself
*"until the measured impact of T1+T2 is known"*, and attributed up to ~25 ms
of a ~39 ms overhead to it. T1 and T2 landed (ADR-223) and the measured
`on_conditions` ratio went from **1.52x to 1.02x**: the overhead that
remained is ~2%, not 25%. Tier 3 is therefore not worth its risk, and it is
**not** implemented. This is the plan's §14 verdict, recorded here because
the plan's rule is that a phase closes with a number.

### 5.2 F1 — the ON dispatch is outlined into a cold function

The trap dispatch — the ON stack scan, the switch over the handlers and the
handler calls — was inline in the procedure, so the hot function's body
carried code that runs once in a hundred iterations. ADR-249 moves it into a
`cold`, `noinline` function of its own, one per condition key.

**The measurement** — `benchmarks/lang/on_dispatch.pli` (the handler runs 1%
of the time), the paired PL/I-vs-C ratio and the hot function's size in the
linked binary:

| build | ratio at `-O2` | `_PLI_ON_DISPATCH` |
|---|---|---|
| before (ADR-223) | 0.95x | 432 bytes |
| after (ADR-249) | 0.95x | **244 bytes** |

**The timing does not move, and that is the finding.** The plan's gate
expected the timing delta to be in the noise and asked for the code density
to be measured instead: the hot function's body — the code the loop runs
through — is **44% smaller**, and the trap code is no longer in it at all.
The `__text` section is unchanged in size (10128 → 10152 bytes): the code
moved rather than shrank, which is exactly what the split is for. One more
call on a path that runs once in a hundred iterations is invisible.

**How to reproduce.** `python3 benchmarks/run_benchmarks.py --only on_dispatch
--runs 5`, and `llvm-objdump -d` on the linked binary for the function
boundaries.

---

## 6. WS-G: PL/I aggregates and arrays (P3)

### 6.1 G1 and G2 — already in the tree

The plan's account of WS-G was stale on both counts, and the gates close
without a diff:

- **G1 (contiguous-array detection).** The compiler already lowers an array
  subscript to an affine offset — a `CHARACTER(10)` array with bounds
  `0:9` gives `%off = sub i64 %i64, 0` and one
  `getelementptr inbounds [10 x i32], ptr %A.addr, i64 0, i64 %off`, with
  **no multiply**. The storage is contiguous by construction, so there was
  nothing to detect.
- **G2 (whole-array assignment as a copy).** `Sema::expandWholeArrayAssign`
  returns early for identical static plain shapes, and the IR is a
  whole-aggregate load and store — not a loop. IBM's four conditions are
  implemented by `Sema::wholeArrayStorageOk` (no DEFINED overlay, no BASED
  storage, no CONTROLLED array, no dynamic member).

`tests/ir/array_stride.pli`, `tests/ir/array_memcpy.pli` and
`tests/core/bad_array_assign_defined.pli` pin all three facts.

### 6.2 G3 — the shape of an array parameter: measured, and reverted

The attribute was implemented (`dereferenceable` on the base pointer of a
by-ref array parameter with a static shape) and priced with a paired
PL/I-vs-PL/I benchmark.

| build | median |
|---|---|
| without the attribute | 52.86 ms |
| with the attribute | 52.53 ms |

**0.99x — noise.** The optimized IR is identical with and without the
attribute, and the linked objects are byte-identical. The reason is
structural: within one compilation unit LLVM sees the call site and the
alloca, so it already knows the shape; and the ENTRY descriptor grammar
accepts only a `*` dimension, so a fixed-size array parameter cannot cross
a compilation unit in this stage — the attribute has no place to act.

The revert is also a soundness decision. `dereferenceable` implies non-null,
and PL/I does not give the compiler that guarantee: a BASED actual may have
a null locator, and the compiler checks neither that nor the conformance of
an actual with its dummy (a ten-element actual for a four-thousand-element
dummy compiles clean). The dummy's extent is a fact about the dummy, not
about the storage behind the pointer. ADR-271 records the rule.

**How to reproduce.** `python3 benchmarks/run_benchmarks.py --only array_param
--runs 5` with the attribute and with `--no-param-attrs`, and
`cmp` on the two linked objects.

---

## 7. WS-H: runtime I/O (P4)

### 7.1 The profile the gate asks for

WS-H's gate is explicit: *"Profile first — if the 1.38x is in the file
syscall path (not formatting), the whole matching/descriptor design is the
wrong fix."* The profile says the formatting is not where the time goes:

| benchmark | what it measures | ratio at `-O2` |
|---|---|---|
| `io_throughput` | list-directed formatting to stdout | **0.79x** (faster than C) |
| `file_io` | the same formatting, through a FILE | **1.37x** |

So H1 (the compile-time format/data-list match), H2 (the per-call-site
descriptor) and H3 (format pooling) would be aimed at code that is already
ahead of its C twin. **They are closed by the profile**, and the item that
pays is H4 — the file path.

### 7.2 H4 — the file read path: 340 ms → 10 ms

Splitting `file_io` into its phases locates the cost:

| phase | PL/I | C twin |
|---|---|---|
| write, 500 000 records | 40 ms | 30 ms |
| **read, 500 000 records** | **340 ms** | **20 ms** |

The read is 17x the C twin's because a list-directed GET pulls the record
one character at a time, and each character is a locked stdio call. The
stdout path has had a slab for this since ADR-220; the FILE path never did.
ADR-272 gives it one — the characters come from a slab that one `fread`
fills — and the read of 500 000 records falls from **340 ms to 10 ms**, the
whole `file_io` program from about **85 ms to about 55 ms** (a clean A/B of
the same two binaries; the harness on a shared host does not resolve the
difference on its own).

The slab belongs to the *file* it was filled from, not to the selected
stream: a GET statement unselects the file when it ends, and the characters
read but not yet consumed have to survive that, or two tasks reading one
file would split a record between them.

The write phase is deliberately left alone — an output slab would have to
drain at the record boundary to keep ADR-233's record-atomicity promise, for
the 10 ms between it and the C twin. The stdin path still reads a character
at a time; the same slab would serve it, but the measurement that justifies
the change is the file path's.

**How to reproduce.** `benchmarks/ir/file_io.pli` split into a write-only and
a read-only program, timed against the C twin's `fprintf`/`fscanf` loop;
`tests/driver/file_slab.sh` is the regression gate and
`tests/hardening/task_input.pli` the concurrency gate.

---

## 8. PL/I constant folding — WS-B (B1, B2, B3, B4)

**The criterion.** The plan's standing rule (`OPTIMIZATION.md` §6.4): *a
technique is not implemented until IR demonstrates the miss and benchmarks show
end-to-end benefit*. Each WS-B technique has its own gate, and B1's gate is
explicit: *"Witness: an array loop with a constant lower bound. If LLVM's SCEV
already collapses the induction variable, close without a diff."*

### 8.1 B1 — constant offset folding: closed without a diff

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

### 8.2 B2 — typed constant folding for `FIXED DECIMAL`

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

### 8.3 B3 — constant-argument built-in lowering

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
(ADR-273); the oracle is what made them visible, which is the argument for
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
for `TRANSLATE` (ADR-273).

### 8.4 B4 — picture conversion folding

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

## 9. WS-C — the IRGen peepholes (C1, C3, C4, C5)

**The criterion.** The plan's standing rule applies to every item: a technique
is not implemented until the IR shows the miss and a benchmark shows the gain.
WS-C is *secondary* to the decision layer (WS-K) after the IBM-precedence
reframe (§1.6 of the plan): a peephole is attempted only when the decision
layer cannot express the transformation. That left four items — C1, C3, C4 and
C5 — and each is recorded here with its own verdict.

### 9.0 The differential check both peepholes share

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

### 9.1 C1 — constant-offset concatenation stores: **implemented**

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

### 9.2 C3 — merge bit tests within one storage object: **implemented, kept**

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
argument that kept ADR-274's picture fold, which also showed no `-O2` gain.

The gates are `tests/ir/combine_tests.pli` (one `icmp eq i32` against the
merged expectation, and no second comparison), `tests/core/bit_merge.pli` (the
merged conjunction equals the two tests taken separately over eight values, and
the two shapes the merge must decline — overlapping masks, and two different
values — keep their own value), and the witness in
`tests/driver/experimental_lowering.sh` (the merged and the separate lowering
agree in the `auto`, `runtime` and `llvm` modes).

### 9.3 C4 — the `optimize_if` contract: **closed as an invariant**

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

### 9.4 C5 — the exponent patch for a scaled `FIXED BINARY`: **landed**

**The gate.** The plan's C5 says: *"If the repo does not support binary scale
at all, **defer** — check `Type` for a binary-scale field first."* The field
existed, but nothing used it, so the patch could not land alone. A3 supplied
the language feature (ADR-284) and every path now agrees:

| where | before | now |
|---|---|---|
| a literal assigned to a scaled `FIXED BINARY` | stored unscaled (only `FIXED DECIMAL` was rescaled, ADR-240) | scaled by 2^q (`binLitRescaled`) |
| `+` between two different binary scales | the raw integers added | the operands rescaled to the larger scale by `convert` |
| conversion to `FLOAT` | the raw integer converted (`sitofp` only) | `sitofp` then a multiply by 2^-q |
| list-directed output | the integer printed | the exact 2^q point printed |

**The verdict.** C5 is the `sitofp` followed by a multiply by `2^-q`, with the
factor from `std::ldexp(1.0, -q)`. The factor is a power of two, so the
multiply is exact and avoids a divide. It is part of ADR-284, not a standalone
ADR-244; that reservation is released and `DEFERRED-WORK.md` D2 is done. The
gate is correctness rather than throughput: `tests/core/scaled.pli` pins the
values and the conversion adds one `fmul` to a path that already did
`sitofp`.

### 9.5 C7 — "test vs. move": **closed, already covered by LLVM**

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

### 9.6 C8 — the infinite loop's condition and the `NOT` of a `BIT(1)`: **closed**

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

**The model.** For `BIT(n>1)` the `NOT` is byte-wise over the whole array; the
model is now written down (ADR-283): the padding bits are indeterminate and
are not part of the value, every value operation ignores them, and `NOT` — the
one operation that writes them — complements the declared bits and normalises
the padding to zero. `tests/ir/bit_padding.pli` pins the lowering (`xor` on the
byte, then the mask that clears the padding bits), and ADR-243/ADR-275 read
the same model for the test merge and the comparison. The C8 deferral is
closed.

### 9.7 C9 — reference-count-driven temp elision: **closed, already covered by LLVM**

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
(the `verify` case of §9.5), and a value that is used twice is materialized
once and read back (the `search` case). LLVM then does exactly what MULTICS's
`need_ref` did — remove the temporary whose value nobody needs.

**The verdict.** Closed without a diff, and no IR check: the property is a
property of SSA, not of this compiler's lowering. Nothing to guard, because
nothing in the compiler decides whether a temporary is needed — LLVM does.

---

## 10. WS-E — PL/I decimal (E1, E2, E3, E4, E5)

**The order the plan fixes.** E3 is the *measurement* change — "it must land
**before** WS-E1 so WS-E1 has a real signal" — and E1's own gate says
"**Measure first.** If < 2%, close without a diff". So E3 went first, and E1
was then decided by its gate rather than by an assumption.

### 10.1 E3 — an exact-decimal baseline for `bcd_decimal`: **implemented** (ADR-248)

**The measurement.** The old C baseline computed the benchmark in `double`,
which cannot represent the values exactly, so the two programs were not
computing the same function: the ratio measured the difference between two
numeric models. The new baseline computes the same function exactly, with
`__int128` scaled arithmetic (the PL/I statements map to the scaled integers
one for one — `i * 19999` at scale 4, the integer division rescaled by 10^4,
the scale-8 products rounded back to scale 4 half away from zero).

| build | ratio before | ratio after |
|---|---|---|
| `-O1` | 2.26x | 0.86x |
| `-O2` | 2.26x | **0.79x** |
| `-O3` | 2.26x | 0.74x |

The corpus moves with it: the C-parity geomean at `-O2` is **0.77x** (it was
0.82x with the artifact in it), and `bcd_decimal`'s parity check is now an
exact match the harness enforces.

**The verdict.** The "accepted floor" is **retired**: the PL/I decimal code is
faster than the *exact* C twin, so there is no gap to close. The parity check
became a gate — the two programs print `579971282735858875227.5000`, byte for
byte, and `bcd_decimal` left the harness's "decimal-vs-FP, by design" list,
so a future divergence fails the run.

### 10.2 E1 — compile-time scale reconciliation: **closed without a diff**

**The gate, applied.** E1 asks for a compile-time decimal scale reconciliation
so the rescale can be folded into the magic multiplier. Its witness was the
ratio against the C baseline — and after E3, `bcd_decimal` is **0.79x** and
`decimal_ops` **0.58x**: the PL/I decimal code is already faster than the
exact C twin, so the technique has no gap to close.

**The premise, checked.** The port's premise is that a *runtime* rescale can
be folded into the magic multiplier. There is no such runtime rescale in this
compiler: the IR of the decimal hot loop is call-free —

```llvm
%bin = mul i128 %cvt, 19999          ; price = i * 1.9999
%bin10 = mul i128 %ld8, %ld9         ; price * qty, at scale 8
%adj = mul i128 5000, %0             ; the rounding adjust
%rn = add i128 %bin10, %adj
%res11 = sdiv i128 %rn, 10000        ; rescaled to scale 4
```

— and the mixed-scale case is just as call-free: `decimal(9,2) +
decimal(9,4)` into `decimal(9,3)` lowers to `mul i64 %res, 100` (the exact
scale-up) and `sdiv i64 %rn, 10` (the rounding scale-down), with **no call**.
Every rescale already carries its factor as a compile-time constant, so there
is nothing to fold.

**The gate.** The plan's own test for E1 — *a table of decimal operations with
different operand scales, whose results must be exact, scale and precision
included* — did not exist, so it is written now:
`tests/core/decimal_scale.pli` is a golden test whose expected output is the
table itself, so the *declared scale* of every result is visible in the
snapshot (`3.75` at scale 2, `2.6250` at scale 4), and every value is also
checked against a hand-computed expectation. It covers the common scale of
`+` and `-`, the sum of the scales in a product, the rounding of an assignment
to a narrower scale (half away from zero), the comparison's rescale, and a
wide product that keeps its digits.

**The verdict.** Closed without a diff, per the gate. The residual — a
*semantically required* rounding of a scale-8 product down to scale 4 — cannot
be removed by a compile-time reconciliation: PL/I rounds the product, not the
operands.

### 10.3 E2, E4, E5 — closed, deferred, and one premise that did not hold

**E2 — decimal precision tightening.** The plan includes it "only if E1's
measurement shows a gap; otherwise close" ✓. E1's measurement shows no gap ✓
→ **closed** ✓.

**E4 — `ROUND` with a dynamic digit count: implemented (ADR-276).** The plan
says "nothing to port — the repo already matches. Record as verified-equal."
**The verification did not confirm that**: MULTICS's `decimal_op_.pl1:283-300`
moves `len1 = p1 - i + n + 1` digits of a *decimal*, exactly, while the repo's
`ROUND` converted its argument to **FLOAT** and called `pli_round(double, n)`.
The difference was observable on a wide decimal:

```pli
x = 1234567890123456.7891;   /* DECIMAL(20,4) */
y = round(x, 2);
```

The exact answer is `1234567890123456.79`; the compiler printed
`1234567890123456.80`, because the value passed through a double.

**The fix.** A `FIXED DECIMAL` argument rounded to a *constant* digit count
keeps its exactness: the result is a decimal with that scale, and the rounding
is the conversion the compiler already performs — the same half-away-from-zero
rescale an assignment uses ✓. The count must be constant (a runtime count
keeps the float path, because the scale of a runtime-rounded value is not
known at compile time) ✓, and the argument must be a decimal ✓.
`tests/core/round_decimal.pli` is the gate: the wide decimal above now prints
`1234567890123456.79` ✓, half away from zero holds in both directions ✓, the
result carries the requested scale ✓, and the runtime count still takes the
float path ✓.

**E5 — narrow-type codegen.** The plan defers it: "only if a narrow-integer
benchmark shows a gap on the target. On x86-64 the expected gain is ~0; on a
32-bit or embedded target it is real. **Defer** — record as gated on a
target-specific measurement." The corpus has no narrow-integer benchmark and
the repo's `intBits()` already picks the narrowest of i32/i64/i128 ✓ → **deferred** ✓, gated on a target-specific measurement ✓.

---

## 11. WS-I — check cost with the checks on (I1, I2)

### 11.1 I1 — range-based check elision with checks ON: **closed with the measurement recorded** (ADR-252)

**The measurement.** The pair, re-measured with the current compiler (arm64
macOS, LLVM 23.1.2, median of five): **1.93x** — M7's criterion (within 1.5x)
is **not met**.

**The breakdown, measured.** Two new pairs compile the same program with one
check kind at a time, so the difference is attributed rather than argued:

| Build | Median (-O2) | Ratio |
|---|---|---|
| check-free (the `-O2` default) | 70.4 ms | 1.00x |
| `--subscript-checks` only | 70.3 ms | **0.99x** |
| `--size-checks` only | 127.9 ms | **1.81x** |
| both | 128.5 ms | 1.83x |

The SUBSCRIPTRANGE checks are **free** — LLVM folds them away — and the SIZE
overflow checks are the **whole** difference. So there is nothing to win on
IBM's half of the item (WS-K7), and the plan's half cannot win either: the
range metadata is sound only once the compiler enforces the *declared
precision*, and when it does, the precision check costs what the storage check
costs today. The ratio improves by the **width of a check, not by its
absence** — which is why the criterion is out of reach for this design.

**Why the item closes rather than stays open.** The plan's exit criterion for
the phase is *"checks-on ratio within 1.5x, **or the item is closed with the
measurement recorded**"*. The measurement is recorded — the ratio, the
attribution, and the ordering the item needs (ADR-252) — and the ordering
names the prerequisite: PL/I's SIZE must be the **declared-precision** check
(rules (60)-(63),(86)), not the storage-width check it was when this section
was written. That conformance item landed as A2 (ADR-282); the range metadata
that would make the check cheap follows it and is D1's work. It is not this
plan's work, and the plan's
own design is unsound until it lands — which is why the ordering is recorded
rather than the code.

**Where the checks actually are.** Reading the *optimized* checks-on module:

| check | cost |
|---|---|
| SUBSCRIPTRANGE (the array read) | **free** — LLVM proves `mod(i,1000)+1` is in `[1,1000]` and folds the comparison away; the check is already inline, so IBM's half of the item (WS-K7) needs nothing here |
| SIZE overflow (`mod+1`, `*3`, `+1`, two 64-bit accumulations) | five `llvm.*.with.overflow` intrinsics per hot iteration — the whole remaining difference |

**Why the range fact cannot remove them.** The plan's design — emit the range
metadata always — is sound only if the range is an invariant. Before A2 it was
not: the SIZE check was the **storage width** check, so a variable declared
`FIXED BINARY(15)` could hold `40000`. ADR-282 closes that at every conversion
into the type and fixes the range's definition: `FIXED BINARY(p)` is
`[-2^p, 2^p-1]`, so `FIXED BINARY(31)` covers the whole i32 range (ADR-252's
`2^30` example was one bit tight). Two consequences remain: at `p = 31`/`63`
the declared range *is* the storage range, so no fact can remove the storage
check; and `stampFixedBinRange` still stamps the one-bit-narrower `p`-bit
range, which D1 must align before it emits a fact. The guard in
`stampFixedBinRange` stays until that work lands.

**The ordering the item needs** (ADR-252): the check must enforce the fact
before the fact is emitted. So the work is:

1. **the precision enforcement** — PL/I raises SIZE when a `FIXED BINARY(p)`
   value exceeds its declared precision (rules (60)-(63),(86)); the
   `FIXED DECIMAL` path already checks the declared precision and the binary
   path checks the storage width. This is a conformance change in its own
   right;
2. **then the range metadata**, after which the storage-width checks become
   unreachable and LLVM folds them away.

**What it can buy.** The values in `checks_on.pli` are bounded at run time but
not at compile time, so the remaining checks are *provably live* (`x * 3`
overflows the i32; the two 64-bit accumulations overflow the i64). The range
facts remove the *storage* checks; the *precision* checks stay and cost about
the same. The ratio improves by the width of a check, not by its absence — so
the 1.5x criterion needs the checks to be *cheaper*, not merely fewer.

**Re-measured after A2 (ADR-282).** `checks_on.pli` declares only
`FIXED BIN(31)` and `FIXED BIN(63)`, so the new declared-precision check adds
nothing to it: the pair is **76.4 ms vs 146.6 ms, 1.92x** (arm64 macOS,
LLVM 23.1.2, median of five), unchanged from 1.93x within noise. That is the
bounded outcome ADR-252 predicted, now with the range defined correctly: the
surviving checks are the storage checks, and at `p = 31`/`63` those are the
precision checks.

### 11.2 I2 — loop fusion: **deferred**

The plan defers it: it needs a dependence analysis, LLVM's loop-fusion pass is
not in the default pipeline, and the repo has no measured multi-pass array
workload. Recorded as gated on a benchmark showing the pattern; not built
speculatively.

---

## 12. WS-K — the IBM decision layer: the measurement pass

**The criterion.** WS-K's items are all gated the same way: *measure the
witness first*. This section records what that pass found: **two** items are
implemented (K1, by a mechanism that was already in the tree, and K3, which
landed with the folding branch), **three** close without a diff (K5, K6, K10),
**three** are deferred with the measurement that would promote them (K2 with
K4, and K8), and two are not built (K9, K11).

### 12.1 K1 — `ORDER`-style storage pinning: implemented, and already there

IBM's rule (`zos:6440-6445`): a variable an ON unit can read must be **in
storage** at a trap point, because the handler runs in the raiser's frame.
The plan's sketch was volatile accesses, and its worry was that pinning a
block's variables would be a pessimization.

The tree already implements the rule, by another mechanism: rule (91) gives an
ON unit a **capture context**, sema collects the automatics the unit touches,
and the establishment stores their *addresses* — so the address escapes, the
optimizer cannot promote the variable, and it stays in storage.

| Measurement | Result |
|---|---|
| The correctness | A loop that writes a local with a `ZERODIVIDE` handler that reads it: the handler sees **100, 200, 300** — the latest values |
| The narrowness | The same program with a handler that does *not* touch the local: **zero** capture slots, and the local is promoted to a register at `-O2` |
| The cost | A 10⁹-iteration loop, pinned against promoted: **1.08–1.11 s** against **1.07–1.14 s** — under 1% |

So the pessimization the plan feared does not fall on the untouched symbols:
the capture set is exactly the reachable set. What the tree lacked was the
*reporting* and the *pins*, and both land in ADR-253: `--print-facts` prints
the pinned automatics (`pinned: X`), `tests/core/order_pin.pli` pins the
correctness and `tests/ir/order_pin.pli` pins the capture set. `volatile` is
not used — the capture context gives the same guarantee without forbidding
every other optimization of the variable.

### 12.2 K2, K4 — the inline-vs-library decision: measured, deferred

The gate: *"close without a diff if the library path is already competitive
for the shapes the repo supports."* The pair `string_search:inline->library`
compiles the same program on both paths:

| Path | Median (-O2) |
|---|---|
| inline (`memchr` + `memcmp`) | **110.1 ms** |
| library (`pli_index`) | **2482.1 ms** |

The library path is **22.55x** slower. The decision layer's value is
therefore not in doubt — it is the difference between 110 ms and 2.5 s — and
the repo's per-op table already picks the inline path wherever one exists.

What K2 asks for *beyond* that is the per-call-site refinement, and one
instance of it has already landed with the folding branch: a constant
alphabet lowers to a membership table rather than a library call (ADR-241).
The remaining scope is the built-ins the repo supports, one predicate each,
seeded with IBM's tables (Tables 41/42/43, `mvs:25226-26045`): a large
change whose measurement — that the library path is 22.55x slower — says
the decision matters. **Deferred**, with the promotion measurement named:
a built-in whose per-call-site predicate would change the chosen path.

K4 (the remark that flags every library-call conversion) is the same change
as K2 by the plan's own account — *"K2 and K4 are one change"* — so it is
deferred with it.

### 12.3 K5 — `REDUCE`, a `BY NAME` assignment as an aggregate move: closed

The witness is a `BY NAME` assignment of a ten-member structure, 20,000,000
times; the C twin copies the structure with one aggregate move, which is what
the technique would produce.

| Build | PL/I | C (aggregate move) | Ratio |
|---|---|---|---|
| `-O2` | **5.47 ms** | 6.00 ms | **0.99x** |

The member-wise copy is as fast as the aggregate move, so there is no gain:
**closed without a diff**. (At `-O0` the ratio is 1.48x — the unoptimized
PL/I copy is slower — but the gate measures the optimized build, and there
the two are equal.)

### 12.4 K6 — `NOOVERLAP`, `memmove` → `memcpy`: closed

The plan's own expectation: *"the expected gain is small — measure before
building. If the timing delta is under 1%, close without a diff."*

The measurement starts earlier: a whole-array copy (`b = a`) emits **neither**
`llvm.memmove` nor `llvm.memcpy` — it is an aggregate load and store
(`load [1000 x i32], ptr %A.addr` followed by the matching store). The
member-wise copy of the whole structure is the same shape (12.3). The
overlapping cases — a `SUBSTR` assignment into its own source — are the ones
that need `memmove`, and they are the ones that keep it.

So there is nothing to switch: the disjoint copies are already aggregate
moves, and the copies that may overlap already use `memmove`.
**Closed without a diff.**

### 12.5 K8 — constant-extent `CONTROLLED`: implemented (ADR-257)

IBM's `RULES(NOLAXCTL)` (`zos:23039-23061`) lets the compiler generate the
address of a CONTROLLED variable directly instead of fetching the runtime
descriptor.

The witness is `benchmarks/ir/controlled_stack.pli`: 1,000,000
allocate/fill/sum/free cycles over a `CONTROLLED` array of 100 elements. It
measures **31.5 ms** at `-O2` against **36.4 ms** for its C twin — **0.87x**,
already ahead of C.

The fetch itself is cheap: a micro-benchmark of 100,000,000 `pli_ctl_addr`
calls (the runtime's `ctlFind` scan) measures **~2 ns** per fetch. The
witness makes **two** fetches per cycle (the address and the length) over
1,000,000 cycles — about **4 ms** of the 31.5 ms, some 13%.

So the gain is real. **The sharper accounting** — the *optimized* code of the
witness (the disassembly, not the emitted IR) makes **two** calls per cycle:
`pli_ctl_addr` twice — once hoisted into the fill loop's preheader and once
fresh after it, because the `ALLOCATE` in between changes the generation. The
`pli_ctl_len` fetch is **not** there: the optimizer removed it, because a
statically bounded CONTROLLED array's bounds check already uses the *declared*
bound, so the live length was dead code.

A compiler-held **anchor** removes one of the two: the `ALLOCATE` returns the
new generation's address, so the refresh is free and both references become
loads; the `FREE`'s refresh costs one call. At ~2 ns a call that is ~2 ns of
the cycle, ~2 ms of the witness's 31.5 ms — **about 6%**.

**Implemented** (ADR-257): the compiler keeps the current generation's
address in a slot of its own, so a reference is a load rather than a call.
The runtime's allocation returns the new address (so the refresh after an
`ALLOCATE` is free), the qualification is module-wide (a nested procedure may
`ALLOCATE` the enclosing procedure's `CONTROLLED` variable, and then the
compiler would not see the generation change), the length is the declared
extent, and a free re-reads the generation from the runtime.

The result: the witness's `pli_ctl_addr` call sites go from **two to one**
per cycle, and its time from **31.5 ms to 30.2 ms** — the ratio against its
C twin is unchanged at 0.87x, since the program was already ahead of C.

### 12.6 K9 — `RESEXP`: not built

Rejected by the plan's §21 as an *optimization* — it changes which conditions
a program raises, so it belongs to the extension track (its own ADR, a
`GRAMMAR-COVERAGE.md` extensions row). ADR-259 stays unwritten.

### 12.7 K10 — `SELECT` via binary search: closed

The gate: *"gate on the repo actually having such a pattern — if `SELECT` is
rare, close without a diff."* The pattern is **not** rare: the corpus has
`tests/corner_cases/select_edge.pli` with twenty `WHEN` arms and
`tests/hardening/select_combo.pli` with seventeen. The new witness
`benchmarks/lang/select_chain.pli` is a `SELECT` over twenty constant `CHAR`
arms, 20,000,000 times; the C twin searches the same constants with a binary
search, which is what a binary-search lowering would produce.

| Build | PL/I (the chain) | C (binary search) | Ratio |
|---|---|---|---|
| `-O2` | **33.5 ms** | 345.6 ms | **0.10x** |

The PL/I chain is **ten times faster** than a binary search: its arms are
inlined character compares, while the C twin pays a `strcmp` call per probe.
A binary-search lowering would make this program *slower*.
**Closed without a diff.**

### 12.8 K11 — `INLIST` / `INARRAY`: recorded, not built

Rejected by §21 (a language feature, not an optimization). The transferable
idea — a `CHAR(n≤4)` comparison as one word compare — belongs to the string
path.

---

## 13. Benchmark parity pass (ADR-314, ADR-315)

**The measurement.** `python3 benchmarks/run_benchmarks.py --runs 5`, macOS
26 arm64, unpinned, LLVM 23.1.3. The C twin is built for the PL/I target
policy (`-mcpu=generic -ffp-contract=off`, ADR-315); result file
`benchmarks/results/benchmark_results_1792952361_532466fe.json`.

| Row | before | after |
|---|---:|---:|
| `bit_fields` | 1.45x | 1.05x |
| `bit_merge` | 1.23x | 0.98x |
| `alias_scope` | 1.16x | 0.98x |
| `mandelbrot` | 1.14x | 1.00x |
| `collatz` | 1.48x | 0.98x |
| `on_conditions` | 1.12x | 0.96x |
| `pure_call` | 1.24x | 0.65x |
| `linked_list` | 1.08x | 1.04x |
| `checks_on` | 1.04x | 0.85x |

IR corpus geomean **0.75x** (gate < 0.90x, 20/20 rows valid); language corpus
geomean **0.57x**. 22 of 32 comparable rows are at or below 1.0x and every row
is within 1.06x. `decimal_ops` stays descriptive at 0.59x.

Tasking (`run_tasking_bench.py --models fiber --runs 5`): `task_loop` 0.69x Go
(gate <= 0.90x holds), `task_spawn` 10.8x Go, down from 24.8x before the
ADR-313 arena.

**What changed.** `BIT(n<=64)` logic and masked tests lower through packed
integers (ADR-314); widening a `BIT(1)` no longer emits invalid IR; the C twin
follows the PL/I CPU/FP policy, and the `on_conditions` and `linked_list` C
twins do the same semantic work as their PL/I twins (ADR-315).
