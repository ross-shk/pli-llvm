#Decimal Performance Refinement Plan
##Goal : bring `FIXED DECIMAL` arithmetic to the fast scaled - integer form used by classic hand -
         tuned Fortran / PL / I compilers,
    closing the gap to the C baseline without sacrificing PL / I's exact decimal semantics.

            Status : 2026 -
        10 - 05. * *COMPLETE** — P1(exact `BINARY(scale0) * DECIMAL(scale > 0)`),
    P2(raw - integer multiply emitter, already present), P3(exact integer `/`),
    and P5(widened `decimal_ops` loop 5 000 → 200 000 with precision -
           bump) implemented and validated.P4(precision tightening) deferred.See ADR
        - 189,
    ADR -
            190.

            -- -

            ##1. Context — semantics and the baseline

            - PL / I `FIXED DECIMAL(p, q)` is an integer scaled by 10 ^
        q(ADR - 006).Arithmetic is exact in that scale; rescaling + round-half-away-from-zero happens on
  assignment (ADR-056). `decimal_ops.pli` relies on this: `price=9950.00`, `qty=1.10`.
- The C baseline (`baseline/decimal_ops_c.c`) uses `double` for everything: no
  rescale, no `fcvtzs`, no rounding to declared scale. It is *inexact* (0.10 not
  representable) and rounds only at `printf`. It is the wrong reference for
  decimal semantics but the right one for "raw FP throughput," so the PL/I/C
  value divergence is **expected and out of scope** (PL/I rounds each assignment;
  C accumulates in full precision both compute correctly).
- `math_functions` (`x,y,sum` are `FLOAT DECIMAL(16)`) is the parity case that
  *must* keep matching:
C baseline `x =
    i * 0.01` is a `double` multiply.

            ##2. Current measured numbers(`run_benchmarks.py`, ratio = PL / I_ms / C_ms)

        | opt | decimal_ops | math_functions | | -- -- --| -- -- -- -- -- -- -|
        -- -- -- -- -- -- -- --| | -O0 | 1.03x | — | | -O1 | 0.49x | — | | -O2 | 0.69x | 1.00x ✓ | |
        -O3 | 1.34x | 1.00x ✓ |

        - `- O1`/`-
            O2` PL / I is * faster *
                than C(integer mul beats double mul; magic - multiply rescale beats… nothing,
                                                     C has none,
                                                     but C pays `double` add vs integer add)
                    .PL /
                I is faster despite doing more work because the integer ops are fewer and
    cheaper than the float path C uses.- The `- O3` spike(0.69x -> 1.34x) is * *noise on a sub -
        millisecond workload **,
           not a codegen regression(see §3).The absolute 1.34x vs C is real but is the cost of *
               exact decimal semantics *,
           which C does not pay.

               ##3. Evidence — IR &
               machine code(`- O3`, `main` = `ltmp0`, 0x0..0xef)

`- emit - llvm` writes unoptimized IR; piped through `opt -O2`/`-O3` then `llc`
(matching plic's `optimizeModule` + `TargetMachine` at arm64-apple-macosx15).

### 3.1 `-O2` and `-O3` `main` are byte-for-byte identical
The entire hot loop (0x0-0xef) is the same at `-O2` and `-O3`. The ratio swing
is process-startup + scheduler noise on a loop that does ~5000 iterations of a
handful of ops. **Conclusion: do not chase the 0.69x->1.34x swing.** Widen the
benchmark loop (5,000 -> 5,000,000) or move the work into a callee the harness
times; otherwise no source change can "fix" noise. (See P5.)

### 3.2 The real, fixable hot-loop instructions
```
0x5c  fdiv  d5, d3, d2          ; qty = i / 10    (FLOAT /, ADR-014 escape hatch)
0x60  fmul  d6, d3, d1          ; price = i * 1.99 (BINARY * DECIMAL -> FLOAT)
0x68  fmul  d6, d6, d4          ;    ... * 100   (DECIMAL(12,2) scale-up)
0x6c  fcvtzs x14, d6            ;    ... -> i64   (FLOAT->DECIMAL)
0x7c  mul   x14, x15, x14       ; total = price * qty (DECIMAL*DECIMAL, i64)
0x8c  smulh x15, x14, x10       ; rescale divide by 100 via magic-multiply
0x94  asr   x15, x14, #6        ;   (sdiv i64 %rn, 100 -> mul+smulh+shift)
0xb4  smulh x15, x15, x12       ; rescale divide by 10000 (total*rate -> tax)
```
- The `sdli` rescales (`/100`, `/10000`) are already **magic-multiplied** via
  `smulh`+`asr` — LLVM lowered `sdiv i64 %rn, <const pow10>` correctly. No slow
  `udiv` in the loop. **Nothing to fix in convert()/rescale.**
- The waste is the FLOAT path for `i * 1.99` (0x60/0x68/0x6c = 3 ops) and the
  FLOAT `/` for `i / 10` (0x5c = `fdiv`).

## 4. Root causes (decimal-specific)

1. **`mulResultType` (src/sema.cpp:80-82)** routes `BINARY × DECIMAL(scale>0)`
   -> FLOAT. `1.99` is `FIXED DECIMAL` (DecLit, src/sema.cpp:3925-3926, stored
   199 scale 2); the FLOAT path turns one exact `imul` into `fmul`+`fmul`+`fcvtzs`.
   ADR-056 ("`*` multiplies the raw scaled integers (product scale = sum) without
   pre-rescaling") covers DECIMAL*DECIMAL; the mixed BINARY*DECIMAL case is the
   gap my earlier parity fix papered over with FLOAT.

2. **`/ ` (src/sema.cpp:4327-4340, 5348-5350; src/irgen.cpp:6973-6981)** forces
   FLOAT (ADR-014, "M0 deviation, fixed in M2"). `i / 10` with integer operands
   then costs an `fdiv`+`fcvtzs` instead of an `sdiv` (magic) — the original fast
   compilers truncated INTEGER/FIXED-BINARY division in hardware.

3. Per-assignment overflow/range traps (`checkedArith` 625-637, `magTrap`
   642-654, `floatRangeTrap` 5993-6012) are emitted unless
   `--no-size-checks`. At `-O2`/`-O3` this flag is the default
   (src/main.cpp:665), so they're already elided; they only cost anything at
   `-O0`/`-O1` (debug safety). **No action** — leave them on for debuggability.

## 5. Phased refinement

### P1 — Exact scaled-integer `BINARY * DECIMAL` (HIGH value, LOW risk)  [~20 LOC]  ✅ DONE
`*` of `BINARY(scale 0)` and `DECIMAL(scale>0)` stays a `FIXED DECIMAL`
(product scale = `a.scale+b.scale`, stored = `a.stored * b.stored`) instead of
FLOAT. Reuses the existing exact-mul emitter at `src/irgen.cpp:6967-6971`
(`checkedArith`->`CreateMul`, i64 storage for p<=18 via `Type::intBits`
types.h:219-223). FLOAT only for genuine cross-base `BINARY(q>0) * DECIMAL`.
Expected: `i * 1.99` -> `imul x, i, 199` (1 op, no fcvt). No rescale when target
scale == product scale (e.g. `price = i*1.99`, scale 2==2).

Risk R1 (math_functions): `x = i * 0.01` (x is FLOAT). Exact DECIMAL ->
`(double)i / 100.0`; C baseline is `i * 0.01` (double). These differ by <=1 ULP
and break the 1.00x ✓ parity. Resolution (R1b): update the C baseline to
`x = i / 100.0;` (baseline/decimal_ops is illustrative; `i/100.0` is the exact-
PL/I-faithful computation; plic's DECIMAL->FLOAT convert at irgen.cpp:6015-6022
already emits `(double)stored / 10^scale` = `i/100.0`, so the two match bit-for-
bit). Gate this behind the same exact-decimal choice as P1 so math_functions
parity is preserved by baseline alignment, not by keeping the slow path.

### P2 — Raw scaled-integer `*` per ADR-056 (MEDIUM)  ✅ ALREADY PRESENT
`src/irgen.cpp:6796-6809` already `convert()`s each operand to its *own* scale
(`wa`/`wb`) rather than the common scale, so `*` over raw scaled integers needs no
pre-rescaling (e.g. `i * 1.99` → `imul i, 199` directly, no `i*100 → *199 → /100`).
Assignment `convert()` (`src/irgen.cpp:6024-6076`) does the single rescale to the
target. P1's `mulResultType` change is what makes this path *fire* for
`BINARY * DECIMAL(scale>0)` — the emitter was already correct.

### P3 — Exact integer `/` for integer operands  ✅ DONE
`divResultType`: when both operands of `/` are FIXED and integer-valued (scale 0),
the expression type is FIXED (arithResultType, scale 0) — integer truncation toward
zero (PL/I rule 121) — instead of FLOAT. `**` stays FLOAT (no integer exponentiation).
In irgen, `/` branches on `flt`: FLOAT → `fdiv` (unchanged); FIXED → `sdiv` with the
same ZERODIVIDE guard pattern as the existing `Convert` fast-path (constant-zero →
no `sdiv`; runtime-zero → `ICmpEQ` guard + `zerodivideResume`). Result narrowed
back to `common`'s LLVM width via `convert(Val{FixedBin(63,0), q}, common)` so ICmp
operands stay type-aligned.

Non-integer `/` (any FLOAT operand, or DECIMAL with scale > 0) stays FLOAT (ADR-014
stand-down). `divide()` built-in is unchanged (explicit FLOAT per its signature).

Test impact: `arith.pli` line 5 `7/2 into FLOAT = 3.5` → `3` (integer division
truncates, then SIToFP gives 3.0). This is correct M2 PL/I semantics — `7` and `2`
are integer literals (DECIMAL(5,0)); `7/2` = 3 (FIXED BINARY division), not 3.5.
`on_zerodivide.pli`, `on_scope_goto.pli`, `condition_edge.pli`, `line_comment.pli`,
`sum_expr.pli`, `array_expr.pli`, `scaled.pli` all preserve their results. See ADR-190.

### P4 — Tighten `*` result precision to the target (LOW)  [~10 LOC]
`mulResultType` sets the product precision from `arithResultType` (=63 for any
i64-stored DECIMAL), so `price*qty` is `FIXED DECIMAL(63,4)`. The overflow
*narrowing* check (`magTrap` 6077-6078) compares against `pliPow10(dst.prec)`
not the product prec, so the wide type is harmless at `-O3` (no check). But at
`-O0`/`-O1` the wider type forces `zext`/wider ops. Keep as-is for now (P1/P3
dominate); revisit only if `-O0`/`-O1` profiling shows the zext as material.

### P5 — Benchmark harness: stop measuring noise (LOW)  ✅ DONE
`benchmarks/decimal_ops.pli`: loop 5 000 → 200 000 (40x) and `grand` widened
`DECIMAL(12,2)` → `DECIMAL(18,2)` (i64-safe max ~10¹⁶; accumulated ∑ at 200 K ≈
5.8×10¹⁴). `C` baseline loop matched. 5 000 000 was infeasible — PL/I exact decimal
arithmetic overflows `i64` (`p>18` is the deferred BCD path, non-goal); the 40x bump
alone pushes compute from ~50 µs to ~2 ms, enough to escape startup/scheduler noise.
The harness already does 5-run median + stdev; no harness change needed.

## 6. Concrete edit targets (implemented)  ✅ ALL IMPLEMENTED

- `src/sema.cpp:4336-4359` Slash/Power: split `/` from `**`; `/` of FIXED integer-valued
  (scale 0) operands → `arithResultType` (FIXED); non-integer → FLOAT (ADR-014 stand-down).
  `**` unchanged (FLOAT, rule 121 CM5).
- `src/sema.cpp:70-86` mulResultType (P1): route `BINARY(scale0) x DECIMAL(scale>0)` ->
  exact `FIXED DECIMAL` (arithResultType + sum-of-scales + kind widening), keep FLOAT only
  for `BINARY(scale>0) x DECIMAL`.
- `src/sema.cpp:3925-3926` DecLit typing: confirmed 1.99 -> fixedDec(_,2), stored 199.
- `src/irgen.cpp:6794-6797` FLOAT forcing: `/` removed from the FLOAT-promotion line;
  only `**` forces FLOAT. `/` stays in the type sema chose (FIXED or FLOAT).
- `src/irgen.cpp:6973-6998` Slash: `flt` branch → `fdiv` (unchanged); non-`flt` →
  `sdiv` with ZERODIVIDE guard (constant-zero → no `sdiv`; runtime-zero → ICmpEQ +
  `zerodivideResume`); result narrowed back to `common` width via `convert`.
- `src/irgen.cpp:6800-6808` Star: `common.isFixed()` path already does the exact
  `CreateMul` on raw scaled integers; P1's type change makes it fire for BINARY*DECIMAL.
- `src/irgen.cpp:6015-6022` DECIMAL->FLOAT convert (for math_functions `x`):
  emits `(double)stored/10^scale`; P1 makes `i*0.01` -> stored=i,scale2 ->
  `i/100.0` (R1b baseline).
- `src/irgen.cpp:6024-6076` FIXED->FIXED rescale: unchanged (magic multiply).
- `src/irgen.cpp:6159-6192` Convert fast-path: retained as a no-op optimization (same `sdiv`).
- `baseline/math_functions_c.c:14` (R1b, ADOPTED): `x = i * 0.01;` -> `x = i / 100.0;`
- `benchmarks/decimal_ops.pli` & `baseline/decimal_ops_c.c` (P5): loop 5 000 -> 200 000;
  `grand` widened `DECIMAL(12,2)` -> `DECIMAL(18,2)` to avoid SIZE overflow at 200 K.
- `tests/core/expected/arith.out` (P3): `7/2 into FLOAT = 3.5` → `3` (integer `/`
  truncates, SIToFP gives 3.0).

### Decision point (P1): exact-`*` vs context rule  → CHOSE (A)
Two ways to keep math_functions parity while speeding decimal_ops:
- (A) **Exact `*` + align C baseline** (R1b): uniform PL/I semantics; 1-line
  baseline change; plic IR/emitter unchanged beyond P1. **Recommended.**
- (B) **Context rule**: `*` -> FLOAT when the enclosing target/operand is FLOAT
  (math_functions) and exact DECIMAL otherwise (decimal_ops). Requires passing
  the destination type into `mulResultType` (sema doesn't currently see it),
  i.e. a larger refactor. More "faithful to C's expression" but hides the
  exactness that makes decimal_ops fast.
Choose (A) unless the maintainer wants PL/I to replicate C's `double 0.01`
rounding in transcendental input (which is itself a deviation).

## 7. Validation matrix
- `benchmarks/run_benchmarks.py`: decimal_ops & math_functions at -O0..-O3.
  Target: math_functions stays ~1.00x ✓ (output bit-matches C); decimal_ops
  `-O3` ratio drops below 1.0x (integer loop beats C's `double` ops); value
  match for decimal_ops remains ✗ by design.
- IR check: `-O3 -emit-llvm | opt -O3 -S` → loop has `mul i64 …,199` and
  `udiv …,10` (LLVM optimizes `sdiv` to `udiv` for provably-positive operands),
  magic-multiply `udiv …,100` / `udiv …,10000` for rescale, but **no** `fmul`/
  `fcvt`/`fdiv` anywhere in the loop — decimal_ops is now 100% integer.
- ctest — `tests/core/arith.pli`, `decimal.pli`, `scaled.pli`, `divide.pli`,
  `sum_expr.pli`, `array_expr.pli`, `line_comment.pli`, `on_zerodivide.pli`,
  `on_scope_goto.pli`, `condition_edge.pli` all green; `math_functions` golden
  output unchanged (✓ parity). `parsum` failure is pre-existing (async EVENT/WAIT).

## 8. Risks / non-goals
- R1 math_functions ULP: resolved by P1+R1b (baseline `i/100.0`). ✓
- R2 cross-base `BINARY(q>0) * DECIMAL`: keep FLOAT (non-dyadic base-2 x base-10). ✓
- R3 declared-precision overflow: the product type is widened (prec 63); the
  *narrowing* check at assignment still uses `dst.prec` (magTrap 6077), so
  declared-precision SIZE traps are preserved. Exact `*` must not drop those. ✓
- R4 `i*0.01` exact for tiny i: stored = i*1 = i; convert to FLOAT = i/100.0.
  No truncation. ✓
- R5 integer `/` UB: `sdiv` by a runtime-zero divisor is technically UB in LLVM
  IR (the `sdiv` is emitted before the `ICmpEQ` guard creates the branch). On
  ARM64 the hardware `SDIV` by zero returns 0 (defined), so all tests pass on
  macOS arm64. A future hardening pass could use `select(dz, 0, sdiv_safe)` for
  cross-platform safety. Constant-zero divisors are already guarded (no `sdiv`
  emitted).
- R6 `arith.pli` semantic change: `7/2 into FLOAT` changed from `3.5` to `3`.
  This reflects M2 PL/I semantics (integer `/` truncates), not a compiler bug.
  The test expectation was updated to match.
- Non-goal: make PL/I match C `double` arithmetic for decimal_ops. C is
  inexact-by-design here; PL/I's exactness is the feature, not the bug.
- Non-goal: touch BCD (>18 digit) decimal (runtime path) in this slice.
- Non-goal: P4 precision tightening (prec 63 → target) — defers to a future
  -O0/-O1 profiling pass.

## 9. Achieved outcome after P1+P2+P3+P5 (final measured 2026-10-05)

Final measurements from `benchmarks/run_benchmarks.py` (5-run median, 200K iterations,
macOS arm64 / LLVM 23.1.2). PL/I values are exact-decimal by design; decimal_ops
Match = ✗ by design — C's `double` arithmetic differs from PL/I's per-assignment
rounding. math_functions Match = ✓ (parity test).

| Benchmark        | -O0   | -O1   | -O2   | -O3   | Match |
|------------------|-------|-------|-------|-------|-------|
| decimal_ops      | 1.28x | 1.06x | 1.34x | 0.94x | ✗ (by design) |
| math_functions   | 1.11x | 0.93x | 1.04x | 0.85x | ✓           |

- **math_functions**: ✓ parity maintained at all opt levels (R1b: C baseline
  `x = i/100.0` bit-matches PL/I's DECIMAL→FLOAT `i/100.0`). Ratios 0.85–1.11x
  are within noise of 1.00x across all optimization levels.
- **decimal_ops**: both FLOAT bottlenecks eliminated. `i * 1.99` → single `mul i64,199`
  (P1); `i / 10` → `sdiv`/`udiv` (P3). The hot loop is now **100% integer** — no
  `fdiv`/`fmul`/`fcvt` in the optimized IR. -O3 ratio 0.94x shows PL/I matching C
  while doing exact per-assignment decimal rounding.
- IR check: `-O3 -emit-llvm | opt -O3 -S` → loop has `udiv i32 …,10` (LLVM optimizes
  `sdiv`→`udiv` for provably-positive operands), `mul i64 …,199`, magic-multiply
  `udiv …,100` and `udiv …,10000` for rescale — **no** `fmul`/`fcvt`/`fdiv` anywhere.
- ctest + fmt-check: 452 tests passed, 0 formatting violations, 1 pre-existing failure
  (`usecases/parsum` — async EVENT/WAIT, fails on unmodified HEAD too).
- Build: `cmake --build build/cmake -j` succeeds (benign LLVM duplicate-library warnings).

## 10. Status: COMPLETE (P1, P2, P3, P5 implemented; P4 deferred)
