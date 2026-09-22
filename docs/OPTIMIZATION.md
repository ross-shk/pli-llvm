# Compiler optimization plan

## 1. Goals and principles

The backend objective is fast generated code without weakening PL/I semantics.
Correctness, diagnostics, and debuggability are constraints on optimization,
not separate modes that may silently change the language.

1. Optimize with PL/I knowledge in HIR/MIR; leave target-independent classical
   optimization and target lowering to LLVM.
2. Preserve facts until their last useful representation. Shapes, extents,
   scales, condition enable-state, alias sets, and storage lifetimes must not be
   erased before the pass that consumes them.
3. Emit analyzable LLVM IR: canonical loops and address arithmetic, explicit
   intrinsics, accurate attributes, and no opaque runtime call where a standard
   LLVM operation expresses the same semantics.
4. Add a custom pass only for a measured PL/I-specific gap that cannot be
   represented for an existing LLVM pass. Every custom pass has a benchmark,
   correctness tests, and an optimization remark.
5. Make profitability target- and profile-aware. Semantic transformations are
   target-independent; code growth, vectorization, unrolling, and inlining use
   LLVM's target and profile models.
6. Never emit a language check that is disabled by the resolved condition
   prefix. Enabled checks may be removed only by proof, without changing which
   condition is raised or the observable order of conditions and side effects.

## 2. Optimization contract

HIR and MIR lowering are mandatory at every optimization level. `-O0` disables
optional optimization passes, not the explicit conversions, checks, descriptor
operations, and control flow required for correct lowering.

Each HIR/MIR operation records the properties needed to transform it safely:

- type, precision, scale, shape, and extent;
- memory effects, possible conditions, and whether an operation may resume;
- alias class, storage identity, and escape/capture information;
- source location and lexical condition enable-state;
- integer overflow and conversion semantics.

Passes use analyses with explicit invalidation rather than running every pass to
a fixpoint. Small canonicalization groups may iterate to a bounded fixpoint.
HIR and MIR verifiers run after construction and in assertion-enabled builds
after each transforming pass.

## 3. Ownership

| Work | Level | Reason |
|---|---|---|
| Constant evaluation and conversion simplification | HIR | PL/I precision, scale, rounding, and conditions |
| Aggregate conformance, fusion, and `BY NAME` | HIR | source shape and name matching |
| Condition enable-state and potential-condition analysis | HIR | lexical and dynamic PL/I semantics |
| Picture specialization | HIR | picture field programs are compile-time objects |
| Loop and address canonicalization | HIR/MIR | expose standard induction and affine forms |
| Check insertion, elimination, and widening | MIR | dominance, ranges, and condition ordering |
| String/aggregate operation selection | MIR | layout, overlap, padding, and length invariants |
| Descriptor simplification | MIR/LLVM | escape and interprocedural argument information |
| Alias, range, effect, and profile facts | MIR to LLVM | optimizer interface |
| CSE, DCE, GVN, SCCP, LICM, SROA, inlining | LLVM | mature general-purpose implementations |
| Vectorization, unrolling, scheduling, ISel, registers | LLVM | target cost model and machine information |

## 4. HIR pipeline

1. **Canonicalization and constant evaluation.** Fold with exact PL/I result
   precision and scale. Simplify conversions only when intermediate rounding,
   truncation, padding, and enabled conditions are unchanged. Propagate an
   `INITIAL` value only while the object is unmodified, unaliased, unescaped,
   and not externally visible.
2. **Shape and aggregate planning.** Resolve conformance and `BY NAME`, then
   represent aggregate expressions as a loop plan. Fuse conformable producers
   and consumers when dependence analysis proves legal; retain temporaries
   where overlap, evaluation order, or condition timing requires them.
3. **Iteration normalization.** Preserve once-only evaluation of `TO`, `BY`,
   and `WHILE`; derive direction and trip counts when proven; produce canonical
   induction and affine subscript expressions. Do not duplicate expressions
   that can raise a condition or have effects.
4. **String planning.** Flatten concatenation trees, propagate symbolic lengths
   and maximum capacities, and select direct assignment/comparison forms.
   Aliasing and overlap remain explicit for MIR operation selection.
5. **Condition and effect analysis.** Attach resolved prefix state and compute
   possible conditions and effects. Remove handler establishment only when no
   reachable operation can raise or explicitly signal the handled condition
   and no separately compiled call can do so.
6. **Picture specialization.** Evaluate constant validation and specialize a
   field program when code size and profile justify it; otherwise retain the
   shared runtime interpreter.
7. **Storage and overlay analysis.** Resolve `DEFINED`/`iSUB` address mappings,
   build alias sets, and compute escapes and lifetimes. Refine a storage class
   only when allocation identity, generation state, `ALLOCATION`, address
   comparison, and condition behavior are unobservable.

## 5. MIR pipeline

1. **CFG construction and simplification.** Build explicit normal and condition
   edges, split critical edges as needed, and remove unreachable blocks.
2. **Check insertion.** Materialize only enabled checks. Keep the source
   condition and resume point attached to each check so later motion cannot
   change observable handling behavior.
3. **Range and check optimization.** Use dominance, scalar evolution, known
   extents, and symbolic string lengths to eliminate redundant checks. Widen a
   loop's checks into a preheader only if overflow-safe range arithmetic proves
   every iteration and moving the condition is not observable.
4. **Aggregate and string lowering.** Select `memcpy` only for equal layouts and
   non-overlapping storage, `memmove` where overlap is possible, and `memset`
   for legal repeated-byte fills. Preserve padding semantics. Lower flattened
   concatenations into one capacity check, one length update, and direct copies
   when failure and alias behavior permit it.
5. **Temporary and descriptor simplification.** Narrow temporary lifetimes and
   scalarize internal descriptors only when fields do not escape and callee
   mutation through the by-reference PL/I interface remains observable. Let
   ordinary LLVM SROA finish the scalar replacement.
6. **Lowering preparation.** Canonicalize affine addresses and loops, outline
   cold condition paths, and assign branch weights from profiles or conservative
   static estimates.
7. **LLVM fact export.** Attach only facts proved for the exact operation:
   TBAA consistent with PL/I overlays and character access, alias scopes only
   between proven-disjoint sets, parameter attributes only across their valid
   lifetime, and range information in forms LLVM accepts. Emit `nuw`/`nsw`,
   `inbounds`, `nonnull`, or `dereferenceable` only where violating the fact is
   impossible on every defined path; otherwise use explicit checked arithmetic
   and ordinary pointers.

The `DEFINED` alias graph prevents false disambiguation within an overlay set;
`!noalias` is used only between sets proved disjoint. Character, `BASED`, and
external accesses remain conservative unless storage provenance proves more.

## 6. LLVM pipeline

Use the new pass manager's standard `PassBuilder` pipeline for the selected
optimization level. Prefer IR shape, standard intrinsics, function attributes,
and LLVM's `FunctionAttrs`, Attributor, SROA, IPSCCP, inliner, loop, and
vectorization passes over local replacements.

Runtime declarations are generated from one ABI table and carry tested memory,
capture, unwind, return, and allocation attributes. Typed runtime entry points
are selected before LLVM lowering when operand types are known. Known copies,
fills, checked arithmetic, and lifetime markers are emitted as LLVM intrinsics
rather than recognized later from compiler-generated call sequences.

Candidate custom passes are deliberately limited:

| Candidate | Placement | Admission criterion |
|---|---|---|
| PL/I check combining | before loop optimization | MIR cannot legally express a profitable cross-block check fact for standard LLVM analyses |
| Descriptor specialization | ThinLTO pre-link/post-link | a stable descriptor ABI blocks measured interprocedural constant propagation |
| Condition-region cleanup | after inlining | LLVM leaves measured EH/cleanup overhead after ordinary CFG and EH simplification |

A candidate is not implemented until representative IR demonstrates the missed
optimization and benchmarks show end-to-end benefit. Passes use stable LLVM
analysis APIs and are tested against the oldest and newest supported LLVM
versions.

ThinLTO is available with `-flto=thin`, full LTO with `-flto=full`, and neither
is silently enabled by `-O2`. Profile-guided optimization supports
instrumentation generation/use; profile data supplies branch weights, indirect
call targets, hot/cold splitting, and inlining guidance. Profile mismatches are
diagnosed. Post-link optimization may be added only with a supported toolchain
and reproducible benchmark evidence.

## 7. User controls

| Flag | HIR/MIR policy | LLVM policy |
|---|---|---|
| `-O0` | mandatory lowering only; preserve debug locations and variables | `O0` |
| `-Og` | cheap canonicalization, folding, and proven check elimination | debug-oriented pipeline |
| `-O1` | local simplification without material code growth | `O1` |
| `-O2` (default) | all generally profitable semantics-preserving passes | `O2` |
| `-O3` | profile/cost-guided fusion and specialization with more code growth | `O3` |
| `-Os` / `-Oz` | avoid or reverse transformations that grow code | `Os` / `Oz` |

Optimization levels never change decimal results, enabled conditions, aliasing,
or I/O behavior. Any future relaxed semantic mode is a separate, explicit flag
and is not implied by `-O3`.

Condition prefixes determine language checks at every level. A diagnostic mode
may instrument additional subscript, string-range, conversion, and arithmetic
failures, but it must be documented as instrumentation because it can make a
previously disabled condition observable. It must not masquerade as an
optimization setting.

## 8. Semantic limits

- `ABNORMAL` objects and synchronization-visible state use the conservative
  accesses and barriers required by their language semantics. `volatile` is
  used only where its LLVM contract is sufficient.
- `REDUCIBLE` permits call optimization only when the complete procedure effect
  analysis supports the corresponding LLVM attributes. It does not by itself
  justify `readnone`, `speculatable`, or removal of a potentially trapping call.
- Stream and record I/O operations retain program order. Formatting work may be
  specialized internally but is not moved across observable I/O or conditions.
- Calls through unknown entries, separately compiled procedures, runtime
  handlers, `BASED` storage, and escaped addresses are assumed to read or write
  all reachable storage unless an ABI summary proves otherwise.
- Optimization must preserve the selected PL/I evaluation order wherever
  conditions, volatile/abnormal data, I/O, allocation, or aliasing make it
  observable.

## 9. Measurement and validation

Optimization work starts from a reproducible baseline and an inspected IR
miss, not from a proposed pass.

- Benchmark scalar and affine array loops, dynamic descriptors, aggregate
  expressions, decimal arithmetic, fixed and varying strings, conditions,
  procedure calls, and representative mixed applications.
- Compare equivalent semantics and safety checks against clang at the same
  optimization, LTO, and profile settings. Track runtime distributions, code
  size, compile/link time, and peak compiler memory; use hardware counters when
  they explain a regression.
- Keep correctness suites for each transformation, including aliasing,
  overflow, zero-trip and negative-step loops, resumable conditions, and
  separately compiled calls. Differential and sanitizer runs cover optimized
  builds.
- Provide LLVM optimization records plus PL/I remarks for missed fusion,
  retained conversions, descriptor escapes, and check elimination. Remarks
  explain the blocking fact and source location.
- Gate changes with statistically stable benchmark thresholds. Keep benchmark
  results and compiler/toolchain versions, not generated LLVM IR snapshots, as
  the long-term performance record.

## 10. Implementation order

1. Establish the benchmark harness, correctness corpus, optimization records,
   and `-O0`/`-O2` baselines.
2. Centralize runtime attributes and improve emitted IR shape so the standard
   LLVM pipeline reaches the baseline target without custom passes.
3. Add MIR CFG, range, effect, escape, and alias analyses; implement check
   elimination with condition-order tests.
4. Implement HIR aggregate/string planning and MIR lowering, then validate
   vectorization and allocation removal in LLVM's optimization records.
5. Add PGO and explicit ThinLTO, including multi-file correctness and profile
   mismatch tests.
6. Admit custom LLVM passes only for measured residual gaps, one pass at a
   time, with an owner and maintenance budget.
