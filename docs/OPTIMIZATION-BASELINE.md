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
