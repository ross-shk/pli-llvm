# MLIR runtime migration plan

## 1. Purpose

Move selected, pure PL/I operations out of opaque C runtime calls when doing so
produces measurably better code, while preserving PL/I behavior and keeping the
C runtime fallback available during rollout.

The migration has two possible implementation paths:

1. Direct LLVM IR lowering with `llvm::IRBuilder<>` for small operations.
2. A narrow, optional `pli` MLIR dialect for an admitted aggregate/vector
   slice, as allowed by ADR-158.

MLIR is not the goal by itself. It is admitted only when measurements show a
remaining optimization or maintainability benefit after the simpler LLVM and
runtime-bitcode options have been evaluated. This follows ADR-158.

The build and full test suite must remain green at every landed change. No
phase may change PL/I semantics, weaken a test, or move parser/sema behavior
into the backend.

## 2. Scope

### In scope

An operation is a migration candidate only if all of the following are true:

- The computation to migrate is completely described by its operands, result,
  and destination buffer. Condition dispatch/resumption remains a separate
  runtime action.
- The migrated computation does not depend on a file cursor, process state,
  runtime condition stack, allocator, task, event, clock, or other hidden
  mutable state.
- Its bounds, string lengths, shape, or scalar type are available in HIR or can
  be passed explicitly without changing the public PL/I ABI.
- Its current runtime implementation has tests that can serve as regression
  evidence against the specification and semantic tests.
- Removing or exposing the call has a measured benefit, or the operation is
  needed to establish a reusable lowering pattern for a measured candidate.

Initial candidates:

| Wave | Candidate operations | Why grouped |
|---|---|---|
| W1 | `assign_char`, `assign_varying`, `high`, `low`, `uppercase`, `lowercase`, `reverse`, `center`, `cmp_char` | Straight-line copy, fill, or elementwise loops |
| W2 | `index`, `verify`, `verify_from`, `search`, `tally` | Search loops, early exit, and 1-based results |
| W3 | `substr`, `substr_assign`, `substr_assign_varying`, `repeat`, `translate`, `trim` | Clipping, blank padding, capacity, and varying-length updates |
| W4 | `mod_ll`, `mod_dd`, degree math variants, supported math wrappers, numeric conversions | Scalar math and conversion checks |
| W5 | Reductions, checked arithmetic, scaled decimal, and BIT operations that are already emitted inline | Direct LLVM cleanup and reuse, not runtime removal |
| W6 | Aggregate copy, array expressions, gathers, and structure fills | Optional only after an ADR-158 performance admission gate |

The list is an inventory, not a commitment to migrate every item. W1-W5 are
direct LLVM candidates. Production MLIR is restricted to W6 unless a new ADR
explicitly supersedes ADR-158. Each operation must pass the admission gate in
section 7.

### Out of scope

Keep these responsibilities in C unless a separate approved plan changes the
runtime architecture:

- `PUT`, `GET`, `EDIT`, `RECORD`, `FILE`, and `STRING` I/O.
- `ON`, `SIGNAL`, `REVERT`, and runtime condition-stack management.
- `ALLOCATE`, `FREE`, `CONTROLLED`, `AREA`, and allocator ownership.
- `WAIT`, `DELAY`, `TASK`, and `EVENT` state.
- `SYSTEM`, `DATE`, `TIME`, and other host services.
- Error and condition-handler bodies such as `pli_signal_error`,
  `pli_subscript_oob`, `pli_zerodivide`, `pli_fixed_overflow`, and
  `pli_conversion`.
- Decimal I/O formatting until decimal representation and formatting have a
  dedicated design.
- Parser, sema, and HIR language expansion. This is a code-generation plan.

Trap sites may eventually be represented by compiler IR, but condition
dispatch and resumption remain runtime services.

## 3. Non-negotiable constraints

1. `IRGen` remains the only backend-aware component. MLIR types and headers
   must not enter the parser, sema, AST, or HIR layers (ADR-002).
2. `runtime/pli_rt_abi.def` remains the source of truth for every retained
   `pli_*` C ABI entry (ADR-032).
3. The specification and semantic tests are the behavioral authority. The
   runtime implementation is a differential regression oracle, not proof that
   existing behavior is correct. Resolve discrepancies against the
   specification before accepting either implementation.
4. An unavailable optional MLIR build must never change normal compiler
   behavior. It may reject an explicitly requested MLIR-only option with a
   clear diagnostic.
5. No unresolved `unrealized_conversion_cast`, illegal dialect operation, or
   unverified LLVM module may leave the compiler pipeline. Verify after MLIR
   translation/linking, runtime-bitcode linking, and optimization before
   object emission.
6. Generated code must preserve overlap behavior. Operations implemented with
   `memmove` today must not silently become non-overlap-safe `memcpy` loops.
7. Lengths and capacities remain distinct. In particular, varying strings
   carry a live length and a maximum capacity; a lowering must not substitute
   one for the other.
8. PL/I indexing, blank padding, condition prefixes, and resumable condition
   values must remain exact. Optimization flags must not change semantics.
9. Existing runtime symbols are removed only after the replacement has been
   default-on for the agreed compatibility window and all in-tree callers are
   gone.
10. Every landed phase is one reviewable change with a green build and tests.
    A failing test may be used locally for test-first development, but it is
    not a valid phase deliverable.

## 4. Verified starting point

The following facts were checked against the current tree:

- `runtime/pli_rt_abi.def` contains 166 `PLI_FN` entries.
- LLVM 23.1.2 and an MLIR CMake package are available on the current macOS
  development host.
- The project does not currently discover or link MLIR.
- `-emit-llvm` intentionally emits raw compiler IR without linked runtime
  bodies.
- Executable generation links runtime bitcode before `optimizeModule` on the
  in-process path.
- Most linked runtime functions are marked `NoInline`; selected math wrappers
  are changed to `AlwaysInline` by the runtime attribute table.
- `MOD` already has a constant-divisor fast path.
- Concatenation already flattens chains into a fresh exact-size buffer and
  emits LLVM memory copies directly; it is not a runtime-migration pilot.
- Most supported math names already prefer LLVM intrinsics where the installed
  LLVM version provides them.
- The test runner does not currently provide per-test compiler flags for IR or
  execution tests. Mode-differential tests therefore need either a small,
  explicit runner extension or a driver test.

These facts matter because a raw `-emit-llvm` call is not proof that the call
remains opaque in optimized executable code. Measurement must inspect the
post-link, post-optimization artifact as well as raw compiler IR.

Recheck the inventory before implementation because symbol counts and line
numbers drift:

```sh
rg -c '^PLI_FN' runtime/pli_rt_abi.def
rg -n 'CreateCall\(runtimeFn|applyRuntimeAttrs|NoInline|AlwaysInline' \
  src/irgen.cpp src/codegen.cpp
rg -n 'pli_(concat|substr|index|verify|mod_)' \
  src/irgen.cpp runtime/rt_string.c runtime/rt_math.c
cmake --build build/cmake --parallel
ctest --test-dir build/cmake
```

Record the exact compiler revision, LLVM version, target triple, optimization
level, and benchmark command with every baseline result.

## 5. Target architecture

### 5.1 Stable front-end boundary

HIR continues to carry typed PL/I operations, conversions, lengths, and member
paths. IRGen remains the only HIR consumer and lowering-policy owner. Any
`src/mlir/` code is an IRGen-private implementation detail; codegen receives
only verified LLVM modules. Backend selection happens inside IRGen:

```text
AST -> sema -> HIR -> IRGen
                         |-- runtime-call lowering
                         |-- direct LLVM lowering
                         `-- optional MLIR lowering -> LLVM IR
```

The result of every path is one verified LLVM module consumed by the existing
runtime linking, optimization, object emission, and executable linking code.

### 5.2 Lowering modes

Use one experimental mode selector rather than independent comma-separated
flags whose combinations are difficult to define and test:

```text
--experimental-lowering=auto      use each operation's current default
--experimental-lowering=runtime   existing pli_* calls
--experimental-lowering=llvm      direct IRBuilder lowering where implemented
--experimental-lowering=mlir      MLIR lowering where compiled and implemented
--experimental-lowering=<operation>:<mode>
                                  override one operation; leave others on auto
```

The command-line default is `auto`. Initially every operation's entry in the
compiled lowering-policy table selects `runtime`; P5 changes those entries one
wave at a time. The other values force a path for differential tests. If a
forced mode does not implement an operation, it uses the runtime path and
reports the fallback under `-v`; it must not fail or silently emit a partial
lowering. Per-operation overrides are the preferred measurement mechanism once
defaults differ between waves. The operation matrix documents each `auto`
selection.

For an operation whose preferred path is MLIR, `auto` selects MLIR only in an
MLIR-enabled build and otherwise selects its tested LLVM or runtime fallback.
Optional build support must never depend on a mode that was not compiled.

`-emit-mlir` is a separate terminal output mode that forces formation of the
MLIR unit and conflicts with explicit `runtime` or `llvm` modes. Under the
helper architecture below, it prints the generated helper module, not a
whole-source MLIR translation. It is available only in MLIR-enabled builds.
Requesting it in another build produces a driver-level diagnostic and a
nonzero exit status.

Before making these flags permanent user-facing interfaces, record the
decision in a new ADR. Experimental names may change during the pilot.

### 5.3 Runtime fallback

The runtime-call path remains the differential regression path throughout
migration; the specification and semantic tests remain authoritative.
`runtimeFn()` and the `.def` entry stay in place while any supported build,
target, or lowering mode can still call the symbol.

Do not annotate `.def` entries with migration state. Keep migration status in
the operation matrix described in P0 so the ABI table remains signature-only,
as required by ADR-032.

### 5.4 MLIR integration boundary

The exact pointer/length bridge is a design question, not settled fact. Resolve
it in the P2 spike before implementing production operations. The narrow
architecture uses generated private MLIR helpers: translate each helper module
to LLVM IR, link it into the current IRGen module, and call or inline it.
Whole-function HIR-to-MLIR lowering would require coverage of every HIR form in
the function and is a separate backend project, outside this plan.

The selected design must specify:

- Ownership of the MLIR and LLVM contexts.
- Representation of `CHARACTER`, `VARYING`, BIT, arrays, and complex values.
- How `{pointer, live length, capacity}` becomes a legal MLIR value.
- Alias and overlap guarantees.
- Data layout, target triple, address spaces, and index width.
- Diagnostic propagation when MLIR verification or pass execution fails.
- Symbol naming and deduplication across multiple source files.
- Where LLVM module linking occurs relative to runtime linking and
  optimization.
- How `-emit-llvm`, `-emit-mlir`, `-c`, and multi-module builds behave.

Do not use `unrealized_conversion_cast` as the final bridge. It may appear
during staged conversion, but the conversion target must reject it before
translation to LLVM IR.

### 5.5 Suggested dialect shape

Keep the first dialect deliberately small:

- A `!pli.buffer`-like value represents pointer, extents, strides, capacity,
  and mutability without losing PL/I aggregate semantics.
- Operations describe stable aggregate/vector semantics rather than exposing
  loop mechanics.
- Traits declare side effects through MLIR memory-effect interfaces.
- Verification checks operand/result types, writable destinations, and
  statically impossible capacities.
- Canonicalization folds empty inputs, zero lengths, and whole-buffer copies
  only where PL/I semantics permit it.
- Lowering produces legal `arith`, `scf`, `memref`, `math`, or LLVM dialect
  operations according to the bridge selected in P2.

Do not add one dialect operation per runtime function automatically. W1-W5 use
direct LLVM where admitted. Add an MLIR operation only when it represents
stable aggregate/vector semantics and has more value than a small direct LLVM
helper.

## 6. Work discipline

Use this loop for every phase and every operation:

1. Identify the current call site and runtime implementation by symbol, not by
   a hard-coded line number.
2. Write down edge cases and overlap/alias assumptions before changing code.
3. Add or identify an execution test that covers those cases.
4. Capture raw LLVM IR, optimized linked IR or optimization remarks, and the
   benchmark baseline.
5. Implement the smallest lowering behind the experimental mode.
6. Run differential execution tests against `runtime` mode.
7. Inspect emitted IR and verify the expected call or loop shape.
8. Measure with the same command and environment as the baseline.
9. Run the phase gates.
10. Update the operation matrix with evidence and the rollout decision.

If the same lowering fails twice, stop and ask, as required by `AGENTS.md`.

## 7. Per-operation admission gate

An operation may enter a production migration wave only after all three
baselines below are evaluated:

| Variant | Purpose |
|---|---|
| Runtime call as shipped | Establish current correctness and performance |
| Runtime bitcode with the candidate body allowed to inline | Determine whether attributes/inlining solve the problem without new IR |
| Direct LLVM lowering | Establish the best simple compiler-owned form |

Production MLIR work is considered only for W6 and is admitted only if at least
one of these is true:

- The direct LLVM implementation is sufficiently complex that structured IR
  materially improves correctness, reviewability, or reuse.
- LLVM still misses a measured optimization that canonical MLIR lowering or a
  targeted MLIR transform can expose.
- Several operations share transformations that would otherwise be duplicated
  in LLVM IRBuilder code.
- The operation is part of the measured aggregate/vector slice allowed by
  ADR-158.

Do not proceed to MLIR if removing `NoInline`, correcting memory effects, or a
small direct LLVM lowering produces equivalent code and the MLIR path has no
demonstrated maintenance benefit. Using MLIR for W1-W5 requires a new ADR that
explicitly supersedes ADR-158; an implementation-boundary ADR alone is not
sufficient.

Minimum evidence for an admission decision:

- A self-checking or golden semantic test.
- Raw LLVM IR for the runtime and candidate path.
- Post-link optimization evidence, preferably `-Rpass`/optimization records or
  preserved IR from the executable path.
- Five-run minimum and median timings for a workload above the benchmark noise
  floor.
- Binary-size delta for the witness program and `plic` itself.
- Compile-time delta for the witness and a representative test subset.

## 8. Phased implementation

### P0 - Inventory, baseline, and test controls

**Goal:** establish reproducible evidence and a single migration ledger before
changing lowering behavior.

Steps:

1. Create an operation matrix in this document or a nearby generated-neutral
   Markdown file with these columns: operation, runtime symbol, purity/memory
   effects, overlap-safe, edge cases, existing tests, witness benchmark,
   runtime-bitcode result, LLVM result or `pending`, MLIR admission, current
   default, and removal status.
2. Audit `kRuntimeAttrs` against each candidate C body. Fix incorrect
   attributes as a separate change before using them as evidence.
3. Select two pilot operations:
   - `assign_char` for copy, padding, overlap, and destination capacity.
   - `index` for nested loops, early exit, empty needles, and 1-based results.
4. Add missing semantic boundary tests without adding lowering-specific
   expectations. Required cases include empty values, zero capacity, clipping,
   overlap where allowed, varying live length versus capacity, and maximum
   practical lengths.
5. Add a driver test or a narrowly scoped runner feature that can compile and
   execute one source under each available lowering mode. Do not add a global
   environment flag that accidentally changes unrelated tests.
6. Record correctness, compile time, runtime, code size, and optimized-code
   baselines for the runtime and inlinable-bitcode variants of both pilots.

Deliverables:

- Completed pilot rows in the operation matrix.
- Reproducible baseline commands and saved result summaries.
- Tests that pass on the unchanged runtime path.
- A written decision on whether each pilot proceeds to the bounded direct LLVM
  experiment in P1.

Acceptance:

```sh
cmake --build build/cmake --parallel
python3 tests/run_tests.py ir
python3 tests/run_tests.py driver
ctest --test-dir build/cmake
```

Stop condition: if the runtime-bitcode/inlining variant closes the measured
gap, retain the runtime implementation and end migration for that operation.

### P1 - Direct LLVM pilot

**Goal:** create the simplest compiler-owned reference lowering for admitted
pilots, with no MLIR dependency.

Steps:

1. Add one small IRGen-owned helper for `assign_char`. Keep it in `irgen.cpp`
   unless extraction clearly improves reuse; do not create a new subsystem for
   one helper.
2. Preserve `memmove` semantics when source and destination can overlap. Use
   LLVM memory intrinsics where they express the operation accurately; use a
   canonical counted loop only where needed.
3. Copy `min(dst capacity, source length)`, blank-pad the destination tail, and
   handle zero lengths without pointer arithmetic that creates an invalid
   inbounds pointer.
4. Select the helper only in `llvm` mode. Leave the runtime call unchanged in
   `runtime` mode.
5. Implement `index` after `assign_char` passes. Preserve empty needle -> 1,
   needle longer than source -> 0, first match -> one-based position, and the
   existing result width/conversion at the call site.
6. Add IR checks for the operation body and `CHECK-NOT` checks for the migrated
   call. Keep separate checks for runtime mode so fallback coverage remains.
7. Run both modes through execution tests and compare stdout, exit status, and
   diagnostics.
8. Repeat the P0 measurements and update the operation matrix.
9. Apply the complete three-variant admission gate and record whether each
   pilot should become a production direct LLVM lowering.
10. Before P2, select one W6 aggregate/vector witness and compare its current
    lowering, a bounded direct LLVM prototype, and normal LLVM
    auto-vectorization. Record the residual gap and obtain the ADR-158
    admission ticket; without it, skip P2 and P4.

Acceptance per pilot:

- Runtime and LLVM modes have identical observable behavior.
- LLVM mode emits no call to the pilot's `pli_*` symbol.
- Runtime mode still emits and executes the original call.
- `llvm::verifyModule` succeeds.
- No unrelated runtime symbol or test expectation is removed.
- The measurement record supports either proceeding or stopping.

Rollback: switch the default to `runtime`; the implementation may remain
behind the experimental mode while the regression is investigated.

### P2 - Optional MLIR architecture spike and ADR

**Goal:** prove a portable, verifiable MLIR-to-LLVM helper bridge without
introducing production PL/I semantics.

This phase is time-boxed and may end with a no-go decision.

Prerequisite: P1 step 10 has demonstrated a W6 residual gap and the maintainer
has approved the ADR-158 admission ticket. Otherwise this phase does not start.

Steps:

1. Add a build option, defaulting to off:

   ```cmake
   option(PLIC_ENABLE_MLIR "Build the experimental MLIR lowering" OFF)
   if(PLIC_ENABLE_MLIR)
     find_package(MLIR REQUIRED CONFIG)
     message(STATUS "Using MLIR from ${MLIR_DIR}")
     list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
     list(APPEND CMAKE_MODULE_PATH "${MLIR_CMAKE_DIR}")
     include(TableGen)
     include(AddLLVM)
     include(AddMLIR)
   endif()
   ```

   Do not hard-code Homebrew paths. A clean build selects LLVM and Clang through
   `CMAKE_PREFIX_PATH` or explicit `LLVM_DIR` and `Clang_DIR`; `MLIR_DIR` may
   additionally select MLIR. Add an explicit configure-time check that LLVM
   and MLIR report the same package version and installation prefix.
2. Keep all MLIR source files under `src/mlir/` and compile/link them only when
   `PLIC_ENABLE_MLIR=ON`.
3. Use ODS/TableGen for the canary dialect and operation; declare generated
   headers and dependencies with the package-provided CMake helpers.
4. Add the smallest synthetic canary needed to exercise parsing, verification,
   pass registration, dialect conversion, LLVM translation, and module
   verification. The canary must not change PL/I output or lower a W1-W5
   operation.
5. Ensure the selected pass pipeline declares legal and illegal dialects and
   fails if conversion is incomplete.
6. Verify the translated LLVM module before linking it into the IRGen module,
   then verify the combined module again.
7. Test MLIR-disabled configuration, MLIR-enabled configuration,
   `-emit-mlir`, an unavailable-mode diagnostic, and a normal non-MLIR build.
8. Measure compiler binary size, clean build time, incremental build time, and
   compile time for a representative source.
9. Write a new ADR recording the helper integration boundary, representation,
   optional-build policy, diagnostics, and rejected whole-function approach.
   Never edit ADR-002 or ADR-158.

Acceptance:

- No MLIR header or type appears outside `src/mlir/` and narrow IRGen glue;
  codegen receives only LLVM modules.
- A default build has no MLIR dependency and behaves exactly as before.
- An MLIR-enabled build uses package-provided CMake targets rather than a
  hand-written library list.
- The canary reaches verified LLVM IR with no illegal operations or unresolved
  conversion casts.
- The ADR and measured dependency costs support continuing.

Stop condition: if helper integration is not maintainable, portable, or
measurably useful, record the no-go result. Direct LLVM work continues
independently in P3.

### P3 - Direct LLVM wave rollout

**Goal:** migrate W1-W5 candidates one operation at a time using the proven
direct LLVM pattern.

Apply this order unless measurements justify a different one:

1. W1 straight-line and elementwise strings.
2. W2 search loops.
3. W3 substring and clipping operations.
4. W4 scalar math and conversions.
5. W5 checked operations and reductions, after a dedicated ADR for the split
   between pure value computation and runtime condition dispatch/resumption.

For each operation:

1. Copy its semantic contract and edge cases into the operation matrix.
2. Add missing behavioral coverage first.
3. Apply the section 7 admission gate.
4. Implement direct LLVM lowering behind its forced mode.
5. Differentially test runtime and LLVM modes.
6. Inspect raw and optimized LLVM IR.
7. Measure the witness workload.
8. Update the matrix and land the operation as one atomic change.

Wave-specific review points:

| Wave | Required checks |
|---|---|
| W1 | Blank padding, overlap, zero length, signed/unsigned byte comparisons, varying live length |
| W2 | Empty needle/set, start below 1, start past end, one-based result, early exit, non-overlapping `tally` matches |
| W3 | 1-based start, clipped source, past-end blanks, destination capacity, gap fill, varying length prefix, overflow-safe size arithmetic |
| W4 | NaN/Inf/signed zero, pure conversion result versus retained runtime condition dispatch, divisor sign, LLVM-version intrinsic availability, degree/radian constants |
| W5 | Pure computation separated from condition signaling, enabled and disabled prefixes, exact resume value, absence of disabled-check IR, decimal scale/precision, BIT tail bits |

Freeze an admitted-wave manifest after evaluating candidates. Operations that
do not pass remain runtime-only and do not block the wave's later default
change.

### P4 - Conditional MLIR aggregate/vector pilot

**Goal:** lower one W6 operation end to end through `pli` MLIR after direct
LLVM waves leave a measured aggregate/vector gap.

Prerequisites:

- P2 accepted the optional MLIR architecture.
- P1 step 10 demonstrates an ADR-158 residual gap.
- The maintainer approves an admission ticket naming the operation, witness,
  target, and success thresholds.

Steps:

1. Define the pilot operation, types, parser/printer support, verifier, memory
   effects, and canonicalizations.
2. Add positive verifier tests and negative tests for malformed operation
   forms. Use MLIR test tooling only in MLIR-enabled builds.
3. Lower the operation through the smallest legal dialect set selected by the
   P2 ADR. Keep target-independent transforms before LLVM conversion.
4. Select MLIR mode with a per-operation override so all other operations keep
   their `auto` policy.
5. Add `-emit-mlir` checks for helper semantics and `-emit-llvm` checks for the
   final call-free form.
6. Differentially execute runtime or LLVM reference behavior against MLIR mode
   using the same inputs.
7. Compare optimized IR, runtime performance, compile time, and code size with
   the predeclared baseline.
8. Run representative alias and boundary cases under AddressSanitizer and
   UndefinedBehaviorSanitizer where supported.

Acceptance:

- Reference and MLIR modes agree on results and condition behavior.
- MLIR and translated LLVM modules verify at every required checkpoint.
- The final optimized module contains no unresolved helper/runtime call that
  the pilot promised to eliminate.
- No unsupported target or non-MLIR build regresses.
- Evidence meets the predeclared success threshold.

### P5 - Default changes

**Goal:** change defaults without removing the proven fallback.

Prerequisites for a wave default change:

- Every operation in the frozen admitted-wave manifest passes differential
  tests; declined operations remain runtime-only.
- Linux, macOS, and Windows correctness CI are green where supported.
- MLIR-enabled CI is green on at least one pinned toolchain if MLIR is used.
- No witness benchmark regresses beyond the predeclared threshold.
- Compiler compile-time and size costs are recorded and accepted.
- The new path has been available experimentally for at least one release or
  the maintainer explicitly approves an earlier flip.

Steps:

1. Change the default for one wave, not all operations globally.
2. Keep `--experimental-lowering=runtime` as a rollback path.
3. Run the full verification matrix in section 9.
4. Update `docs/ARCHITECTURE.md` to describe the implemented pipeline, not the
   planned one.
5. Record the default change and compatibility window in the operation matrix
   and release notes.

Rollback criteria:

- Any semantic mismatch or condition-handling regression.
- A target-specific LLVM/MLIR verification failure.
- A material compile-time, runtime, or binary-size regression outside the
  accepted threshold.
- An unsupported build configuration losing the runtime fallback.

Rollback changes only the default. It does not require reverting the tested
lowering implementation.

### P6 - Runtime cleanup

**Goal:** remove obsolete C code only when it is no longer part of a supported
fallback or ABI.

Steps:

1. Confirm with `rg` that no compiler or test path references the symbol.
2. Confirm the operation has been default-on through its compatibility window.
3. Retire the per-operation `runtime` override before deleting its symbol. A
   direct request for a retired `<operation>:runtime` mode must diagnose that
   the reference path is no longer available. The whole-program `runtime` mode
   must report and use `auto` for retired operations.
4. Decide whether external users can depend on the internal `pli_*` symbol. If
   that ABI has shipped as externally usable, use a deprecation cycle rather
   than immediate deletion.
5. Remove the C body, `.def` entry, declarations, attributes, and focused
   runtime-only tests in separate reviewable changes. Preserve semantic tests
   against the compiler lowering.
6. Rebuild runtime bitcode for all supported targets and verify no stale symbol
   remains.
7. Measure runtime archive/bitcode and executable size before and after.

Never delete a runtime body in the same change that first introduces its
replacement.

### P7 - Documentation and finalization

1. Update `docs/ARCHITECTURE.md` sections 2, 3, and the runtime-interface
   section with only the pipeline that actually landed.
2. Do not change grammar-coverage rule status for a codegen-only migration.
   Add test references only if coverage genuinely increased.
3. Add or update the new ADRs created by P2, P3/W5, and P4. Existing ADRs remain
   immutable.
4. Document how to configure an MLIR-enabled build using `MLIR_DIR` or
   `CMAKE_PREFIX_PATH`, including the required LLVM/MLIR version match.
5. Remove experimental flags only after their replacement/default policy is
   documented and tests no longer depend on them.
6. Run the final verification matrix and record test counts and benchmark
   deltas.

## 9. Verification matrix

Run focused checks during development and the full suite once at each phase or
wave boundary.

| Area | Required evidence | Pass condition |
|---|---|---|
| Configure without MLIR | Normal CMake configure/build | No MLIR package required |
| Configure with MLIR | CMake configure with matching `MLIR_DIR` | Package found, generated files build |
| Driver behavior | Mode and `-emit-mlir` driver tests | Valid modes work; unavailable mode diagnoses |
| Runtime fallback | Raw LLVM IR and execution in runtime mode | Original call and behavior remain |
| Direct LLVM | IR checks and execution in LLVM mode | Candidate call absent; module verifies |
| MLIR syntax | `-emit-mlir` checks | Expected `pli` operation and types appear |
| MLIR lowering | Conversion tests and final LLVM IR | No illegal dialects/casts; promised call absent |
| Semantics | Differential execution across modes | Output, status, and diagnostics match |
| Conditions | Enabled/disabled prefix tests | Trap/resume behavior and zero-cost disabled path match |
| Memory safety | Boundary tests plus sanitizer pilot | No OOB, overlap, lifetime, or UB finding |
| Optimization | Post-link optimized IR or optimization record | Expected event is present; final module verifies |
| Performance | Fixed five-run min/median benchmark | Meets the predeclared threshold |
| Compile cost | Clean/incremental build and source compile timing | Cost recorded and accepted |
| Size | `plic`, runtime, and witness binary size | Cost recorded and accepted |
| Regression | `ctest --test-dir build/cmake` | Entire suite passes |
| Quality | `cmake --build build/cmake --target check` | Format, tidy, and warnings gates pass |

Suggested phase-boundary commands:

```sh
cmake --build build/cmake --parallel
python3 tests/run_tests.py ir driver
ctest --test-dir build/cmake
cmake --build build/cmake --target check
```

For MLIR-enabled validation, use a separate build directory so the default
configuration is tested independently:

```sh
cmake -G Ninja -S . -B build/mlir \
  -DPLIC_ENABLE_MLIR=ON \
  -DCMAKE_PREFIX_PATH=/path/to/llvm \
  -DMLIR_DIR=/path/to/llvm/lib/cmake/mlir \
  -DLLD_ROOT=/path/to/lld \
  -DLLD_INCLUDE_DIR=/path/to/lld/include
cmake --build build/mlir --parallel
ctest --test-dir build/mlir
```

Do not use raw `-emit-llvm` alone as optimization evidence. Preserve or inspect
the post-runtime-link module, use optimization remarks, or inspect the final
object code because the normal executable pipeline links runtime bitcode before
optimization.

## 10. Operation checklist

Copy this checklist into the review description for each migrated operation:

- [ ] Runtime symbol and C implementation identified.
- [ ] PL/I rules and semantic edge cases identified.
- [ ] Memory effects and overlap behavior audited.
- [ ] Existing tests listed; missing boundary tests added.
- [ ] Runtime, inlinable-bitcode, and direct-LLVM baselines recorded.
- [ ] MLIR admission decision recorded as accepted, rejected, or not applicable.
- [ ] Fallback path retained and tested.
- [ ] New lowering has focused IR checks.
- [ ] Differential execution passes in every available mode.
- [ ] Raw and post-link optimized IR inspected.
- [ ] Module/dialect verification passes.
- [ ] Performance, compile-time, and size deltas recorded.
- [ ] Full phase gate passes.
- [ ] Operation matrix and implemented-architecture docs updated.

## 11. Decision log and open questions

Resolve these in order; do not let them become implicit implementation choices:

1. Which pilot remains slow after runtime attributes and bitcode inlining are
   corrected?
2. What numeric thresholds admit direct LLVM or MLIR and reject regressions for runtime,
   compile time, and binary size?
3. Does generated-helper integration provide a sufficiently clean verified
   boundary, or should MLIR be rejected for this compiler architecture?
4. What is the canonical MLIR representation for PL/I aggregate buffers,
   including extents, strides, capacity, and mutability?
5. Which targets and LLVM/MLIR versions receive MLIR-enabled CI coverage?
6. Is the `pli_*` ABI internal-only in all shipped artifacts, or does symbol
   removal require external deprecation?
7. How long must a new default remain available before its C fallback can be
   removed?

Recommended initial policy:

- Set quantitative thresholds from P0 data rather than guessing them here.
- Use `assign_char` and `index` as complementary direct LLVM pilots.
- Keep MLIR optional and off by default through P2/P4.
- Keep the runtime fallback for at least one release after each wave becomes
  default, unless the maintainer explicitly approves otherwise.
- Do not schedule P2 or P4 until the W6 witness and direct LLVM baseline from
  P1 step 10 show a residual aggregate/vectorization gap that satisfies
  ADR-158.

## 12. Execution order

```text
P0 inventory and evidence
 -> P1 direct LLVM pilots
 -> P3 direct LLVM waves -> P5 defaults -> P6 cleanup -> P7 docs
 `-> optional P2 MLIR infrastructure spike
       `-> conditional P4 W6 aggregate/vector pilot -> P5
```

P0 and P1 are required. P3 direct LLVM work does not depend on P2. P2 may
conclude that MLIR is not justified without blocking P3, defaults, cleanup, or
documentation. P4 requires both an accepted P2 architecture and a measured W6
admission ticket. Rejecting MLIR is a successful outcome: the purpose of the
plan is better, maintainable generated code, not adoption of a particular
framework.
