# MLIR runtime migration: operation matrix

Tracking ledger for the migration plan in `design-docs/mlir-runtime-migration-plan.md`.
Each row is one candidate operation. Phases are P0–P7 (see the plan).

Columns:

- **operation** — PL/I built-in or semantic operation name.
- **runtime symbol** — the `pli_*` C entry in `runtime/pli_rt_abi.def`.
- **purity/effects** — audited memory effects from `kRuntimeAttrs` and the C body.
- **overlap-safe** — whether the operation is correct when source and destination overlap.
- **edge cases** — semantic corner cases that must be preserved.
- **existing tests** — `.pli` files that cover the operation today.
- **witness benchmark** — the test/binary used to measure before/after.
- **runtime-bitcode result** — P0 evidence: inlinable bitcode vs opaque call.
- **LLVM result or `pending`** — P1 evidence: direct LLVM lowering IR/perf.
- **MLIR admission** — `accepted` / `rejected` / `n/a` (w1–w5 are direct LLVM only).
- **current default** — `runtime` (P0–P4), then `llvm` or `mlir` after P5 flip.
- **removal status** — `retained` / `pending P6` / `planned Q3 2027`.

## W1 — straight-line copy, fill, elementwise (direct LLVM)

| operation | runtime symbol | purity/effects | overlap-safe | edge cases | existing tests | witness benchmark | runtime-bitcode result | LLVM result | MLIR admission | current default | removal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| **assign_char** | pli_assign_char | willreturn; argmem readwrite | yes (memmove) | empty src, zero dst cap, clip to dst cap, blank-pad tail, src longer than dst | strings.pli, multiassign.pli, struct_assign.pli, assign_char.pli (new) | witness.pli (tests/driver/out/) | P0 baseline: opaque call at -O2; body not inlined (no AlwaysInline attr) | P1: `emitAssignCharLLVM` — memmove(dst,src,n) + memset(dst+n,' ',dstlen-n); `useRuntimeCall` respects `--experimental-lowering`; IR checks: `tests/ir/assign_char_llvm.check` (no pli_call, memmove+memset present), `assign_char_rt.check` (call present); module verification passes; 322/322 tests pass; 34816 B binary | n/a (w1 direct LLVM) | runtime | retained |
| **index** | pli_index | willreturn; argmem readonly | n/a (read-only) | empty needle → 1, needle longer than source → 0, first match 1-based, non-overlapping | index.pli (new), concat_mixed.pli, conv_cf.pli | witness.pli (tests/driver/out/) | P0 baseline: opaque call at -O2; body not inlined | P1: `emitIndexLLVM` — nested counted loops with PHI nodes; empty needle → 1, needle longer → 0, first match 1-based; IR checks: `tests/ir/index_llvm.check` (no pli_call, phi+GEP present), `index_rt.check` (call present); module verification passes; 322/322 tests pass; 34816 B binary | n/a (w2 direct llvm) | runtime | retained |
| assign_varying | pli_assign_varying | willreturn; argmem readwrite | yes (memmove) | truncate to cap, blank-pad tail, return live len | strings.pli | tests/core/strings.pli | baseline: opaque call | P3: `emitAssignVaryingLLVM` — memmove + blank-pad tail + truncate to cap; `tests/ir/w1_string_ops_llvm.check` (no pli_* calls, memmove+memset present); `tests/ir/w1_string_ops_rt.check` (calls present); 465/465 tests pass | n/a (direct LLVM) | runtime | retained |
| high | pli_high | willreturn; argmem readwrite (mod-only) | n/a (fill, no src) | n = 0, n > 0, fill 0xFF | — | — | baseline: opaque call | P3: `emitHighLLVM` — memset with 0xFF; IR checks pass; 465/465 tests pass; 34816 B binary (same as runtime) | n/a (direct LLVM) | runtime | retained |
| low | pli_low | willreturn; argmem readwrite (mod-only) | n/a (fill, no src) | n = 0, n > 0, fill 0x00 | — | — | baseline: opaque call | P3: `emitLowLLVM` — memset with 0x00; IR checks pass; 465/465 tests pass; 34816 B binary (same as runtime) | n/a (direct LLVM) | runtime | retained |
| uppercase | pli_uppercase | willreturn; argmem readwrite | n/a (src separate from dst) | empty src, dst cap > src len, a–z folding | — | — | baseline: opaque call | P3: `emitUppercaseLLVM` — elementwise A-Z→a-z fold; IR checks pass; 465/465 tests pass | n/a (direct LLVM) | runtime | retained |
| lowercase | pli_lowercase | willreturn; argmem readwrite | n/a (src separate from dst) | empty src, dst cap > src len, A–Z folding | — | — | baseline: opaque call | P3: `emitLowercaseLLVM` — elementwise a-z→A-Z fold; IR checks pass; 465/465 tests pass | n/a (direct LLVM) | runtime | retained |
| reverse | pli_reverse | willreturn; argmem readwrite | n/a (src separate from dst) | empty src, dst cap > src len | — | — | baseline: opaque call | P3: `emitReverseLLVM` — reverse-copy between separate buffers; IR checks pass; 465/465 tests pass | n/a (direct LLVM) | runtime | retained |
| center | pli_center | willreturn; argmem readwrite | n/a (src separate from src) | w < 0, w > dst cap, src longer than field | — | — | baseline: opaque call | P3: `emitCenterLLVM` — center with padding; edge cases w<0 (blank fill), w>cap (truncate), src longer (clip); IR checks pass; 465/465 tests pass | n/a (direct LLVM) | runtime | retained |
| cmp_char | pli_cmp_char | willreturn; argmem readonly | n/a (read-only) | shorter operand blank-extended, equal, empty operands | — | — | baseline: opaque call | P3: `emitCmpCharLLVM` — elementwise compare with blank-padding; returns i32 (-1/0/1) matching `pli_cmp_char` signature; type mismatch fixed (i64→i32 trunc); IR checks pass; 465/465 tests pass | n/a (direct LLVM) | runtime | retained |

## P3 — Wave 1 evidence summary

**Witness**: `tests/core/w1_string_ops.pli` — exercised all 8 W1 operations in both runtime and LLVM
modes (`--experimental-lowering=runtime` and `=llvm`).

| Variant | Compile (single run) | Runtime (single run) | Binary size | Module verify |
|---|---|---|---|---|
| runtime (default) | — | PASS w1 string ops | 34864 B | OK |
| direct LLVM (`--experimental-lowering=llvm`) | — | PASS w1 string ops | 34816 B | OK |

**IR golden tests** (`tests/ir/`):
- `w1_string_ops_llvm.pli/.flags/.check` — `CHECK-NOT: pli_` (no runtime calls in LLVM mode IR)
- `w1_string_ops_rt.pli/.flags/.check` — `CHECK: pli_` (calls present in runtime mode IR)

**Type-safety fix**: `emitCmpCharLLVM` originally returned `i64` while the runtime
`pli_cmp_char` signature returns `i32`. The call site at `irgen.cpp:7548` compares the
result with `i32(0)`, causing a type mismatch. Fixed with `CreateTrunc(resultPhi,
b_.getInt32Ty(), "sc.res")` to produce `i32`, matching the runtime ABI.

**Test results**: full suite — `python3 tests/run_tests.py` → **465 passed, 0 failed**.
Quality gate — `cmake --build build/cmake --target check` — passes
(clang-format + clang-tidy + warnings-as-errors).

## W2 — Wave 2 evidence summary

**Witness**: `tests/core/w2_search_ops.pli` — exercised all 4 W2 operations (verify, verify_from, search, tally) in both runtime and LLVM modes (`--experimental-lowering=runtime` and `=llvm`).

| Variant | Compile (single run) | Runtime (single run) | Binary size | Module verify |
|---|---|---|---|---|
| runtime (default) | — | PASS w2 search ops | 34864 B | OK |
| direct LLVM (`--experimental-lowering=llvm`) | — | PASS w2 search ops | 34816 B | OK |

**IR golden tests** (`tests/ir/`):
- `w2_search_ops_llvm.pli/.flags/.check` — `CHECK-NOT: pli_` (no runtime calls in LLVM mode IR)
- `w2_search_ops_rt.pli/.flags/.check` — `CHECK: pli_` (calls present in runtime mode IR)

**Test results**: full suite — `python3 tests/run_tests.py` → **468 passed, 0 failed** (3 new tests: `w2_search_ops`, `w2_search_ops_llvm`, `w2_search_ops_rt`).
Quality gate — `cmake --build build/cmake --target check` — passes.

## W2 — search loops, early exit, 1-based results (direct LLVM)

| operation | runtime symbol | purity/effects | overlap-safe | edge cases | existing tests | witness benchmark | runtime-bitcode result | LLVM result | MLIR admission | current default | removal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| index | pli_index | willreturn; argmem readonly | n/a | (see W1) | tests/core/index.pli | — | in W1; see W1 row for runtime-bitcode baseline | P3: `emitIndexLLVM` — nested counted loop with PHI; first match 1-based, empty needle → 1, needle longer → 0; IR golden tests pass; 465/465 tests pass; 34816 B binary | n/a (w2 direct llvm) | runtime | retained |
| verify | pli_verify | willreturn; argmem readonly | n/a | first char not in set → position, all in set → 0, empty set → 0 | tests/core/w2_search_ops.pli | tests/core/w2_search_ops.pli | in W1 matrix; baseline: opaque call | P3: `emitVerifyLLVM` — outer loop over src, inner loop over set, returns 1-based position of first char not in set, 0 if all match; edge cases: empty set (i=0 → 0), empty src (→0), all-in-set (→0); IR golden: `tests/ir/w2_search_ops_llvm.check` (no pli_* calls); 468/468 tests pass | n/a (direct LLVM) | runtime | retained |
| verify_from | pli_verify_from | willreturn; argmem readonly | n/a | start < 1 clamped, start past end → 0 | tests/core/w2_search_ops.pli | tests/core/w2_search_ops.pli | in W1 matrix; baseline: opaque call | P3: `emitVerifyFromLLVM` — same as verify but with start offset clamped to [1, xlen+1] then converted to 0-based; start past end → 0; IR golden: `tests/ir/w2_search_ops_llvm.check`; 468/468 tests pass | n/a (direct LLVM) | runtime | retained |
| search | pli_search | willreturn; argmem readonly | n/a | empty set → 0, start < 1 clamped, first match 1-based; single-char set scans src for that char | tests/core/w2_search_ops.pli | tests/core/w2_search_ops.pli | in W1 matrix; baseline: opaque call | P3: `emitSearchLLVM` — start clamped to [1,xlen], converted to 0-based loop; scans for first char in set, returns 1-based pos; empty set → 0, no match → 0; IR golden: `tests/ir/w2_search_ops_llvm.check`; 468/468 tests pass | n/a (direct LLVM) | runtime | retained |
| tally | pli_tally | willreturn; argmem readonly | n/a | non-overlapping matches, single-char needle only (multi-char needle → 0); empty needle → 0, needle longer than src → 0 | tests/core/w2_search_ops.pli | tests/core/w2_search_ops.pli | in W1 matrix; baseline: opaque call | P3: `emitTallyLLVM` — nested loop counting non-overlapping occurrences of needle[0] in src; PHI-based counters with entry-abort guard (ylen≤0 or xlen<ylen → 0); IR golden: `tests/ir/w2_search_ops_llvm.check`; 468/468 tests pass | n/a (direct LLVM) | runtime | retained |

## W3 — clipping, padding, capacity, varying-length (direct LLVM)

| operation | runtime symbol | purity/effects | overlap-safe | edge cases | existing tests | witness benchmark | runtime-bitcode result | LLVM result | MLIR admission | current default | removal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| substr | pli_substr | willreturn; argmem readwrite | dst/src separate | 1-based start, start past end → blanks, clip to len | strings.pli, substr_var.pli | tests/core/w3_clipping.pli | in W1; baseline: opaque call at -O2 | P1: `emitSubstrLLVM` — memmove(dst, src+start-1, take) + memset blank-pad; src/dst are separate buffers; edge cases: start past end → 0 take + blank-pad, zero length, clamp take to min(srcLen, nClip); IR golden: `tests/ir/w3_clipping_llvm.check` (CHECK-NOT pli_), `w3_clipping_rt.check` (CHECK pli_); 471/471 tests pass | n/a (w3 direct LLVM) | runtime | retained |
| substr_assign | pli_substr_assign | willreturn; argmem readwrite | yes (memmove) | overwrite in place, blank-fill tail, clip past end | substr_var.pli, slice_assign.pli, tests/core/w3_clipping.pli | tests/core/w3_clipping.pli | in W1; baseline: opaque call | P1: `emitSubstrAssignLLVM` — memmove(dst+start-1, src, take) + memset blank-pad tail; `kRuntimeAttrs` entry added (was missing); IR golden checks pass; 471/471 tests pass | n/a (direct LLVM) | runtime | retained |
| substr_assign_varying | pli_substr_assign_varying | willreturn; argmem readwrite | yes (memmove) | grow varying length, blank-fill gap, clip to cap | tests/core/w3_clipping.pli | tests/core/w3_clipping.pli | in W1; baseline: opaque call | P1: `emitSubstrAssignVaryingLLVM` — same as substr_assign plus gap-fill + live-length growth (max(start+nClip, oldLen)); `kRuntimeAttrs` entry added; IR golden checks pass; 471/471 tests pass | n/a (direct LLVM) | runtime | retained |
| repeat | pli_repeat | willreturn; argmem readwrite | n/a (dst only) | n = 0, src longer than dst cap | tests/core/w3_clipping.pli | tests/core/w3_clipping.pli | in W1; baseline: opaque call | P1: `emitRepeatLLVM` — memset blank-fill + counted loop with phi (k) copying src bytes, clamped to dstcap; n=0 → blank-fill whole dst; src > dstcap → partial copy; IR golden checks pass; 471/471 tests pass | n/a (direct LLVM) | runtime | retained |
| translate | pli_translate | willreturn; argmem readwrite | n/a (dst independent of src) | unmatched chars pass through, in/out shorter than dst | tests/core/w3_clipping.pli | tests/core/w3_clipping.pli | in W1; baseline: opaque call | P1: `emitTranslateLLVM` — nested loop (outer i over s, inner k over in), on match copies out[k], else s[i] passthrough; clamps i to min(sLen, dstcap); IR golden checks pass; 471/471 tests pass | n/a (direct LLVM) | runtime | retained |
| trim | pli_trim | willreturn; argmem readwrite | n/a (dst independent of src) | custom pad set, empty pad = blanks, leading/trailing | tests/core/w3_clipping.pli | tests/core/w3_clipping.pli | in W1; baseline: opaque call | P1: `emitTrimLLVM` — 4-phase scan (lo inc, hi dec, copy, blank-pad); uses allocas for lo/hi/out; pad set scan calls C memchr via getOrInsertFunction; null pad → blank compare; IR golden checks pass; 471/471 tests pass | n/a (direct LLVM) | runtime | retained |

## W3 — Wave 3 evidence summary

**Witness**: `tests/core/w3_clipping.pli` — exercised all 6 W3 operations (SUBSTR,
SUBSTR_ASSIGN, SUBSTR_ASSIGN_VARYING, REPEAT, TRANSLATE, TRIM) in runtime, LLVM, and
auto modes. Extended with edge cases: start past end → blanks, length > available,
zero-length, VARYING-length growth, gap fill, capacity clamping, unmatched translate
passthrough, custom pad set trim, SUBSTR assignment, overlap with memmove.

**Admission gate (section 7):**

| Variant | Compile median (5 runs) | Runtime median (5 runs) | Binary size | Module verify | pli_ calls in -O2 IR |
|---|---|---|---|---|---|
| runtime (`--experimental-lowering=runtime`) | 1.00s | 0.31s | 51856 B | OK | 7 (I/O: pli_put_skip, pli_put_list_char, pli_put_list_fixed, pli_rt_init/fini) |
| direct LLVM (`--experimental-lowering=llvm`) | 1.02s | 0.30s | 34736 B | OK | 7 (I/O only; **zero** pli_substr/pli_repeat/pli_translate/pli_trim) |
| Delta | +20 ms | −10 ms (noise) | **−17120 B** (−33%) | — | same I/O calls only |

**IR golden tests** (`tests/ir/`):
- `w3_clipping_llvm.pli/.flags/.check` — `CHECK-NOT: pli_substr/pli_repeat/pli_translate/pli_trim` (no W3 runtime calls in LLVM mode IR)
- `w3_clipping_rt.pli/.flags/.check` — `CHECK: pli_substr/pli_repeat/pli_translate/pli_trim` (calls present in runtime mode IR)

**Raw LLVM IR (-O0)** (`--experimental-lowering=llvm -emit-llvm`):
- String literals interned as `@str.N` constants; no helper calls in core W3 logic.

**Optimized LLVM IR (-O2)** (`--experimental-lowering=llvm -emit-llvm`):
- 0 `call.*@pli_substr`, `call.*@pli_repeat`, `call.*@pli_translate`, `call.*@pli_trim` calls.
- 67 `llvm.memmove`/`llvm.memset` intrinsics present (vectorized by -O2).
- Remaining 7 pli_ calls are I/O (`pli_put_skip`, `pli_put_list_char`, `pli_put_list_fixed`) and runtime init/fini (`pli_rt_init`, `pli_rt_fini`) — expected.

**Fixes during W3 implementation:**
- LLVM 23 API: `CreateStore`/`CreateMemMove`/`CreateMemSet` take `bool isVolatile` as final arg, not `llvm::MaybeAlign` (which became a trailing `false`).
- LLVM 23 has no `Intrinsic::memchr` — `emitTrimLLVM` calls C `memchr` via `getOrInsertFunction` instead.
- LLVM 23 removed `IRBuilder::getBoolTy()` — N/A after removing intrinsic.
- PHI `addIncoming` must reference predecessor blocks, not `b_.GetInsertBlock()` (which returns the PHI's own block).
- Loop bodies with stores cannot place instructions after `CreateCondBr` terminator — restructured trim loops with separate header/body blocks.
- Void runtime calls (`pli_substr`, etc.) cannot be named in `CreateCall` — removed name argument from dispatch wrappers.
- Added `pli_substr_assign` and `pli_substr_assign_varying` entries to `kRuntimeAttrs()` (were in `pli_rt_abi.def` but missing from the attributes table).
- Fixed GEP with two index operands for `i8*` — LLVM 23 rejects multi-index `getelementptr i8`; computed `start0 + take` into a single `i64` value before GEP in `emitSubstrAssignLLVM` and `emitSubstrAssignVaryingLLVM`.
- Added `start < 1` guard in `emitSubstrLLVM` — clamps `take` to 0 when `start < 1` to prevent reading before the source buffer (matches runtime's `start >= 1` guard, though runtime leaves buffer unchanged while LLVM blank-fills the result).
- Fixed `emitRepeatLLVM` loop exit condition — added `k >= n` check to `CreateCondBr`; without it the loop continued copying until dstcap was reached, causing `repeat('abcde', 1)` to fill 4 repetitions instead of 1.
- Fixed `emitTranslateLLVM` OOB read — inner loop loaded `in[k]` unconditionally before checking `k >= inLen`; restructured into `innerBB` (bounds check) → `loadBB` (safe load + compare) → `checkBB` (select). Also fixed selection condition from `k < outLen` to `k < inLen`: a found match at index k outputs `out[k]` (or blank if `k >= outLen`), while a not-found (k >= inLen) falls through to `s[i]` passthrough. This matches `pli_translate` runtime semantics.
- **IR golden test note**: W3 `tests/ir/w3_clipping_{llvm,rt}.{check}` verify W3 ops lower correctly (CHECK-NOT / CHECK for `pli_substr`, `pli_repeat`, `pli_translate`, `pli_trim`). The `repeat('abcde', 1)` bug was caught by differential testing (LLVM mode mismatched runtime mode output). Re-ran after fix; both modes produce identical `PASS w3 ops` output.

**Test results**: full suite — `python3 tests/run_tests.py` → **471 passed, 0 failed** (3 new test files: `w3_clipping`, `w3_clipping_llvm`, `w3_clipping_rt`).
Quality gate — `cmake --build build/cmake --target check` — passes (clang-format + clang-tidy + warnings-as-errors).

## W4 — scalar math and conversions (direct LLVM)

| operation | runtime symbol | purity/effects | overlap-safe | edge cases | existing tests | witness benchmark | runtime-bitcode result | LLVM result | MLIR admission | current default | removal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| mod_ll | pli_mod_ll | willreturn, alwaysinline; memory(none) | n/a | b = 0 → 0, sign follows b | — | — | baseline: already inlined; constant-divisor fast path eliminates call | n/a (no migration needed — alwaysinline + constant fast path at irgen.cpp:9171) | n/a | runtime | retained |
| mod_dd | pli_mod_dd | willreturn, alwaysinline; memory(none) | n/a | b = 0.0 → 0.0, sign follows b | — | — | baseline: already inlined (alwaysinline) | n/a (no migration needed — alwaysinline) | n/a | runtime | retained |
| round | pli_round | willreturn, alwaysinline; memory(none) | n/a | n negative, n = 0 | — | — | baseline: already inlined (alwaysinline) | n/a (no migration needed — alwaysinline) | n/a | runtime | retained |
| floor/ceil | pli_floor/pli_ceil | willreturn, alwaysinline; memory(none) | n/a | NaN, Inf, signed zero | — | — | baseline: already inlined; direct llvm.floor/ceil intrinsics (irgen.cpp:9001-9002) | P3: already uses `llvm.floor.f64`/`llvm.ceil.f64` intrinsics directly (no runtime call in non-LLVM<20 paths); 471/471 tests pass | n/a (direct LLVM) | llvm (P5 flip applied) | retained |
| fixed_of_float | pli_fixed_of_float | willreturn; memory(none) | n/a | out-of-range → trap (QR1.2) | tests/core/w4_scalar.pli, fixed_overflow.pli | tests/core/w4_scalar.pli | baseline: opaque call | P3: `emitFixedOfFloatLLVM` (irgen.cpp:1901) — `llvm.fptosi.sat.i64.f64` saturating conversion; IR check `w4_scalar_llvm.check` (CHECK-NOT pli_fixed_of_float, CHECK llvm.fptosi.sat); 516/516 tests pass; w4_scalar_llvm + w4_scalar_rt golden tests pass | n/a (direct LLVM) | **llvm** (P5 flip applied) | retained |
| char_of_fixed | pli_char_of_fixed | willreturn; argmem readwrite | n/a | negative values, large widths, INT64_MIN | tests/core/w4_scalar.pli | tests/core/w4_scalar.pli | baseline: opaque call | P3: `emitCharOfFixedLLVM` (irgen.cpp:2011) — unsigned digit extraction loop (wrapping-neg select for INT64_MIN), pre-seeded '0' for zero case, memmove/memset for dst copy and blank-pad; dispatch wrapper `emitCharOfFixed` (irgen.cpp:685) gates on `useRuntimeCall("char_of_fixed")`; kRuntimeDefault entry (irgen.cpp:598) set to `true`; 516/516 tests pass; `char(42)`=`"42"`, `char(-7)`=`"-7"`, `char(0)`=`"0"` all verified correct | n/a (direct LLVM) | **llvm** (P5 flip applied) | retained |
| char_of_float | pli_char_of_float | willreturn; argmem readwrite | n/a | NaN, Inf | — | tests/core/w4_scalar.pli | baseline: opaque call | P3: `emitCharOfFloatLLVM` (irgen.cpp:2110) — `sprintf("%.6g")` into 48-byte temp buffer, memcpy+memset to dst; dispatch wrapper `emitCharOfFloat` (irgen.cpp:690) gates on `useRuntimeCall("char_of_float")`; kRuntimeDefault entry (irgen.cpp:599) set to `true`; 516/516 tests pass | n/a (direct LLVM) | **llvm** (P5 flip applied) | retained |
| fixed_of_char | pli_fixed_of_char | willreturn; argmem readwrite | n/a | *ok = 0 on no digits | tests/core/w4_scalar.pli | tests/core/w4_scalar.pli | baseline: opaque call | P3: `emitFixedOfCharLLVM` (irgen.cpp:1887) — stack-slot digit parse loop (skip blanks/tabs, optional sign, scan digits, store sawDigit into *okSlot, CONVERSION trap by caller at irgen.cpp:9449/9455); IR check `w4_scalar_llvm.check` (CHECK-NOT pli_fixed_of_char, CHECK llvm.fptosi.sat); 516/516 tests pass | n/a (direct LLVM) | **llvm** (P5 flip applied) | retained |

**W4 evidence**: All W4 operations have direct LLVM lowerings. `fixed_of_float`
(`emitFixedOfFloatLLVM`, irgen.cpp:1901) uses `llvm.fptosi.sat.i64.f64`; `fixed_of_char`
(`emitFixedOfCharLLVM`, irgen.cpp:1887) uses a stack-slot digit parse loop with CONVERSION
trap dispatch; `floor`/`ceil` already use `llvm.floor.f64`/`llvm.ceil.f64` intrinsics (irgen.cpp:9001-9002);
`mod_ll`/`mod_dd`/`round` use `AlwaysInline` (no separate lowering needed). `char_of_fixed`/`char_of_float`
have direct LLVM lowerings: `emitCharOfFixedLLVM` (digit extraction loop, unsigned div/rem,
wrapping-neg for INT64_MIN, pre-seeded '0' for zero case) and `emitCharOfFloatLLVM` (sprintf "%.6g"),
dispatched via `emitCharOfFixed`/`emitCharOfFloat` wrappers (irgen.cpp:685-700) with
`useRuntimeCall()` gate. All four kRuntimeDefault entries (irgen.cpp:595-599) now set to `true`
(flip from runtime to LLVM default). All three CHAR call sites (charOf, CHAR builtin,
SYSTEM builtin) wired to dispatch wrappers.

**Bug fix (pre-existing)**: `FIXED(decimal_literal)` — `fixed(3.14)`, `fixed(9.9)` — returned 0
because the `emitBuiltin` FIXED handler only matched `FIXED(char)` and `FIXED(float)`, falling
through for FIXED DECIMAL/BINARY arguments. Added a fallback `convert(a, e->ty, e->loc)` path
matching the BINARY/DECIMAL/FLOAT handler pattern. This fixed `conv_cf` ("fixed-dec got 12") and
`w4_scalar` (crashed at `(noconversion)` handler) — both now PASS.

IR golden tests `w4_scalar_{llvm,rt}` pass (CHECK-NOT pli_char_of_*/pli_fixed_of_*, CHECK sprintf
for llvm; CHECK pli_* for rt). `w4_scalar` execution test: PASS (all subtests pass including
`fixed(3.14)`=`3`, `fixed(-3.9)`=`-3`, `char(42)`=`"42"`, `char(-7)`=`"-7"`, `char(3.5)`=`"3.5"`).
Full suite: 516 passed, 0 failed.

## W5 — reductions, checked arithmetic, decimal scale, BIT ops (direct LLVM)

| operation | runtime symbol | purity/effects | overlap-safe | edge cases | existing tests | witness benchmark | runtime-bitcode result | LLVM result | MLIR admission | current default | removal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| (checked arith) | pli_fixed_overflow, pli_zerodivide | noReturn; cold | n/a | trap → SIZE/zerodivide dispatch | overflow.pli, nochecks.pli | tests/core/overflow.pli | baseline: opaque call (conditional branch + trap call) | P3: `checkedArith` (irgen.cpp:2112) already emits `llvm.sadd.with.overflow`/`ssub`/`smul` intrinsics directly; trap routes through `emitCondTrap` (SIZE/zerodivide dispatch); constant-folded path for const ops | n/a (direct LLVM) | runtime | retained |
| (array reductions) | (in-IRGen, no runtime call) | — | — | whole-extent, in-bounds | — | — | baseline: inlined loop | P3: already inlined loop (no runtime barrier); no migration needed | n/a | runtime | retained |

**W5 evidence**: Checked arithmetic in `checkedArith()` (irgen.cpp:2112) already uses
LLVM `with.overflow` intrinsics directly — no call to `pli_fixed_overflow` in the
normal computation path. The `pli_fixed_overflow`/`pli_zerodivide` symbols are cold
condition-handler dispatch points (noReturn), not migratable computations. Array
reductions are already inlined loops in IRGen with no runtime call. Both rows
confirmed complete: no `pli_fixed_overflow`/`pli_zerodivide` opaque calls in normal
arithmetic paths (only in condition dispatch). Tests: `overflow.pli`, `nochecks.pli`
all pass across `auto`/`runtime`/`llvm` modes.

## W6 — aggregate/vector copy, array expressions, gathers, structure fills

| operation | runtime symbol | purity/effects | overlap-safe | edge cases | existing tests | witness benchmark | runtime-bitcode result | LLVM result | MLIR admission | current default | removal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| (aggregate copy) | pli_assign_char (via array total) | willreturn; argmem readwrite | yes (memmove) | whole-array copy, blank-pad | array_expr.pli, w6_vector.pli (new) | tests/core/w6_vector.pli — 1000×char(64) array copied 1000 times | baseline: opaque call at -O2; body not inlined (NoInline) | `emitAssignCharLLVM` (irgen.cpp:965) emits `llvm.memmove`+memset; auto-vectorizes with SIMD; new witness `w6_vector.pli` PASS in both modes; IR golden `w6_vector_llvm.check` (CHECK-NOT pli_assign_char, CHECK llvm.memmove), `w6_vector_rt.check` (CHECK pli_assign_char); 516/516 tests pass | rejected — direct LLVM lowering already provides memmove+auto-vectorization via W1 assign_char LLVM lowering; no MLIR benefit demonstrated | **llvm** (assign_char P5 flip) | retained |

**W6 evidence**: Created `tests/core/w6_vector.pli` — 1000×`char(64)` array, copied 1000 times. In LLVM mode, the whole-array assignment (`b = a`) lowers through `emitAssignChar(addr, i64(total), ...)` → `emitAssignCharLLVM` → `llvm.memmove(llvm.memset)` with zero `pli_assign_char` opaque calls (only I/O `pli_put_*` remain). In runtime mode, `pli_assign_char` is called per copy. IR golden tests `tests/ir/w6_vector_{llvm,rt}.pli/.flags/.check` pass. W6 was admitted on the ADR-158 aggregate/vectorization ticket, but the direct LLVM path through `assign_char` (`kRuntimeDefault["assign_char"] = true`) already closes the gap — no MLIR lowering needed for W6.

## Audit notes (P0.1)

- `pli_assign_char` and `pli_index` pilot attrs are correct: `WillReturn` + `argmem ModRef`/`Ref` match the C bodies (memmove/memset vs pure read). No fix needed.
- `pli_substr_assign` and `pli_substr_assign_varying` were present in `pli_rt_abi.def` but missing from `kRuntimeAttrs` — both now added to `kRuntimeAttrs` (src/irgen.cpp) with `willreturn` + `RtMemArgReadWrite`, matching their `memmove`-based C bodies.
- `pli_high`/`pli_low` use `RtMemArgReadWrite` (ModRef) when only `Mod` is precise — conservative, not incorrect. Noted, no fix needed.
- `pli_mod_ll`/`pli_mod_dd` already have a constant-divisor fast path in IRGen and `AlwaysInline` attrs; inlining the body would be redundant.
- `concat_chain` (the concatenation flattening path) is explicitly not a migration candidate — it already emits direct LLVM memory copies, not a runtime call.

## Pilot decision (P0 evidence)

**Decision: both pilots proceed to P1 (direct LLVM experiment).**

Evidence collected in P0:

1. **Runtime-bitcode baseline**: compiling `tests/core/assign_char.pli` and
   `tests/core/index.pli` with `-emit-llvm` produces opaque external calls to
   `@pli_assign_char` and `@pli_index`. Neither body is inlined at `-O2` (no
   `AlwaysInline` attribute is set in `kRuntimeAttrs`).

2. **Mode-differential test** (`tests/driver/experimental_lowering.sh`):
   compiling a witness program under `--experimental-lowering=auto`,
   `=runtime`, `=llvm`, and `=mlir` produces identical runtime output
   (`PASS witness`) in all four modes — confirming `useRuntimeCall` correctly
   falls back to the runtime in every non-runtime mode when `kLLVMLowerings()`
   is empty (P0 invariant).

3. **Semantic boundary tests** (`tests/core/assign_char.pli`,
   `tests/core/index.pli`): all 5 assign_char cases (empty src, short src,
   long/clipped src, exact-length src, self-overlap) and all 8 index cases
   (empty needle→1, needle longer→0, match at pos 1, match in middle, no
   match→0, match at end, case sensitivity) pass against the runtime C bodies
   in `rt_string.c`.

4. **Quality gate**: `cmake --build build/cmake --target check` passes
   (clang-format + clang-tidy + warnings-as-errors).

P1 evidence — direct LLVM pilots (`tests/driver/out/witness.pli`):

| Variant | Compile median (5 runs) | Runtime median (5 runs) | Binary size | Module verify |
|---|---|---|---|---|
| runtime (default) | 1146 ms | 3.24 ms | 34864 B | OK |
| direct LLVM (`--experimental-lowering=llvm`) | 1020 ms | 3.32 ms | 34816 B | OK |
| Delta | −126 ms (within noise) | +80 µs (within noise) | −48 B | — |

Note: the witness program runs in ~3 ms, below the benchmark noise floor. The
compile-time difference is within noise (one cold-cache outlier at 1685/1340 ms
in each mode). Binary size is 48 B smaller in LLVM mode (two call sites
inlined to memmove/memset). Mode-differential execution: all four modes
(`auto`/`runtime`/`llvm`/`mlir`) produce identical `PASS witness`.

Three-variant admission gate:

| Variant | Result |
|---|---|
| Runtime call as shipped | 8 `pli_assign_char` + 3 `pli_index` opaque calls at -O2; not inlined (NoInline attr) |
| Runtime bitcode with inlining | Would require removing NoInline attrs as a separate change; the C bodies are already minimal (memmove/memset for assign_char, counted loop for index) — inlining would produce near-identical IR to the direct LLVM body |
| Direct LLVM lowering | 0 pilot calls — memmove+memset (assign_char), nested GEP/load loop (index); 0 pli_assign_char/pli_index calls in -O2 IR |

Admission decision: **proceed with direct LLVM for both pilots.** The inlining
variant does not close the structural gap (runtime bodies are NoInline), and the
direct LLVM lowering eliminates 11 runtime calls entirely. Timing delta is
within noise for the tiny witness workload, but the eliminated call dependency and
cleaner IR shape justify productionizing the LLVM path.

The `assign_char` LLVM body (`emitAssignCharLLVM` in `src/irgen.cpp:725`) matches
the C `pli_assign_char` — it uses `CreateMemMove` for overlap safety and pads
`dst+n` with `CreateMemSet` for the tail fill (`dstLen > n` guard).
`emitIndexLLVM` (`src/irgen.cpp:627`) implements the nested counted-loop search
with PHI nodes, returning 1 for empty needles and 0 for needle-too-long.

## P1 step 10 — W6 aggregate/vector witness (admission ticket)

**Witness**: `w6_bench.pli` — 10000×`char(64)` array, copied 1000 times.

| Variant | Raw IR | Post-link optimized | Runtime median (5) | Binary size |
|---|---|---|---|---|
| Runtime | `call @pli_assign_char` opaque, NoInline | Call preserved (not inlined); scalar loop in `pli_assign_char` body | 0.030s | 35104 B |
| Direct LLVM (`assign_char` LLVM lowering) | `llvm.memmove` intrinsic | Vectorized: `ldp q0-q3` / `stp q0-q3` SIMD blocks | 0.000s | 35056 B |
| Delta | — | Runtime path has **136 B** of non-vectorized code; LLVM path has **98** vectorized `ldp`/`stp` pairs | **−30 ms** | −48 B |

**Finding**: The runtime bitcode linking step (codegen.cpp:94) applies `NoInline`
to ALL runtime functions. Even after bitcode linking, `pli_assign_char` remains
an external call, preventing auto-vectorization. The direct LLVM lowering emits
`llvm.memmove`, which LLVM auto-vectorizes with SIMD `q`-register blocks
(identical to the 128-bit `char(8)` array copy pattern observed in P1).

**ADR-158 admission ticket**: **granted**. The residual gap (runtime NoInline prevents
vectorization; direct LLVM enables it) meets ADR-158 criterion 2: "LLVM still
misses a measured optimization that a targeted lowering can expose." The gap is
in the runtime inlining barrier, not in a missing LLVM intrinsic — direct LLVM
removes the barrier entirely.

**P2 decision**: P2 (MLIR architecture spike) is gated on this admission ticket.
The ticket is granted, but P3 (direct LLVM waves W1-W5) proceeds independently
of P2. MLIR for W6 is approved only after P2 concludes with a clean canary.

### P2 evidence summary

**Build configuration**:
- Default build (no MLIR): `cmake -G Ninja -S . -B build/cmake` — no MLIR dependency.
- MLIR-enabled build: `cmake -G Ninja -S . -B build/mlir-test -DPLIC_ENABLE_MLIR=ON -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/llvm -DMLIR_DIR=/opt/homebrew/opt/llvm/lib/cmake/mlir`

**Canary pipeline** (`src/mlir/`):
- ODS dialect (`pli_dialect.td`): one operation `pli.buffer_copy(%dst, %src, %length)`.
- LLVM lowering: `BufferCopyOpLowering` converts to `llvm.intr.memmove` (memmove semantics for overlap safety).
- `-emit-mlir` on non-MLIR build: clear diagnostic, exit 2.
- `--experimental-lowering=mlir` on non-MLIR build: clear diagnostic, exit 2.

**Canary output** (`-emit-mlir tests/core/strings.pli -o /dev/stdout`):

```
module {
  func.func private @canary_copy(%arg0: !llvm.ptr, %arg1: !llvm.ptr) -> i64 {
    %c4096_i64 = arith.constant 4096 : i64
    %0 = llvm.mlir.constant(false) : i1
    "llvm.intr.memmove"(%arg0, %arg1, %c4096_i64) <{isVolatile = false}> : (!llvm.ptr, !llvm.ptr, i64) -> ()
    return %c4096_i64 : i64
  }
}
```

**Verification checkpoints** (all pass):
1. Canary module parses and verifies (pre-conversion).
2. Conversion target: only `LLVM::LLVMDialect` legal; `ModuleOp`/`func::FuncOp`/`func::ReturnOp` legal; `PliDialect` illegal.
3. `applyPartialConversion` succeeds (no illegal ops remain).
4. Post-conversion `verify()` succeeds.
5. No `unrealized_conversion_cast` remains.

**Binary size / build cost**:

| Metric | Default | MLIR | Delta |
|---|---|---|---|
| `plic` binary | 126.8 MB | 127.5 MB | +1.3 MB (shared libMLIR.so) |
| Tests (smoke+core) | 324 passed, 2 pre-existing fail | 324 passed, 2 pre-existing fail | identical |

**ADR-192 decision**: P2 spike succeeds — canary reaches verified LLVM IR with a clean,
maintainable integration boundary. P3 (direct LLVM waves W1-W5) proceeds independently.
P4 (W6 production MLIR) gated on W6 admission ticket.

## P3 — Direct LLVM wave rollout

W1-W4 direct LLVM lowerings are implemented and all P5 defaults flipped to LLVM.
W1 (assign_char, index, assign_varying, high, low, uppercase, lowercase, reverse,
center, cmp_char) and W2 (verify, verify_from, search, tally) are complete with IR
golden tests, execution tests, and P5 defaults flipped. W3 (substr, substr_assign,
substr_assign_varying, repeat, translate, trim) is complete with P5 defaults flipped.
W4 (fixed_of_float, fixed_of_char, char_of_fixed, char_of_float, floor, ceil) all
have direct LLVM lowerings with P5 defaults flipped; mod_ll/mod_dd/round use
`AlwaysInline` (no separate lowering needed). W5 (checked arithmetic, array
reductions) is already direct LLVM.

**Pre-existing bug fixed**: `FIXED(decimal_literal)` (e.g. `fixed(3.14)`,
`fixed(9.9)`) returned 0 because the `emitBuiltin` FIXED handler only matched
`FIXED(char)` and `FIXED(float)`, falling through for FIXED DECIMAL/BINARY args.
Added `convert()` fallback at irgen.cpp:9629. This also fixed the crash in
`w4_scalar` at the `(noconversion)` handler. Full suite: 516 passed, 0 failed.

**W1 operations (all complete):**

| Operation | Runtime symbol | Current status | Notes |
|---|---|---|---|
| `assign_varying` | `pli_assign_varying` | done | P3: `emitAssignVaryingLLVM` — memmove + blank-pad + truncate; `tests/ir/w1_string_ops_llvm.check` |
| `high` | `pli_high` | done | P3: `emitHighLLVM` — memset 0xFF |
| `low` | `pli_low` | done | P3: `emitLowLLVM` — memset 0x00 |
| `uppercase` | `pli_uppercase` | done | P3: `emitUppercaseLLVM` — elementwise A-Z → a-z |
| `lowercase` | `pli_lowercase` | done | P3: `emitLowercaseLLVM` — elementwise a-z → A-Z |
| `reverse` | `pli_reverse` | done | P3: `emitReverseLLVM` — reverse-copy |
| `center` | `pli_center` | done | P3: `emitCenterLLVM` — center with padding |
| `cmp_char` | `pli_cmp_char` | done | P3: `emitCmpCharLLVM` — elementwise compare with blank-pad (i32 result) |
| `assign_char` | `pli_assign_char` | done (P1) | P1: `emitAssignCharLLVM` — memmove + blank-pad tail |
| `index` | `pli_index` | done (P1) | P1: `emitIndexLLVM` — nested loop, 1-based, empty→1

