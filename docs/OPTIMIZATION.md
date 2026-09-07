# Optimization

## 1. Principle

> Optimizations that depend on PL/I semantics happen in HIR/MIR.
> Classical optimizations are LLVM's job, and our output is shaped so LLVM can
> do them.

The historical PL/I optimizers (the IBM Optimizing Compiler's common
subexpression elimination, loop-invariant motion, subscript strength
reduction) are subsumed by LLVM. What LLVM *cannot* do is anything that
requires knowing PL/I's rules: that a `VARYING` string's bytes beyond its
current length are dead, that two `DEFINED` names overlay each other, that a
`SUBSCRIPTRANGE` check is disabled by a prefix, or that an aggregate assignment
is a fusible loop nest.

A second principle: **never emit a check the language does not require.**
PL/I's condition prefixes (rules 60–63) are compile-time information; the
enable-state is resolved in sema, so disabled checks are never generated rather
than generated-and-deleted.

## 2. Where each optimization lives

| Optimization | Level | Why not LLVM |
|---|---|---|
| Conversion-chain shortening | HIR | needs the PL/I conversion lattice |
| Aggregate expression fusion | HIR | shape comes from declarations |
| `BY NAME` resolution | HIR | name matching is a source-level rule |
| Picture specialisation | HIR | picture is a compile-time program (ADR-017) |
| Dead `ON`-unit elimination | HIR | dynamic scoping analysis |
| Check-enable propagation | HIR | lexical prefix semantics |
| Decimal scale propagation | HIR | scale is a type property |
| Bounds-check elimination | MIR | needs dope-vector/extent facts |
| Varying-string length propagation | MIR | length is a language invariant |
| Concatenation tree flattening | MIR | avoids O(n²) temporaries |
| Aggregate copy → `memcpy` | MIR | layout equality is a PL/I question |
| Descriptor scalarisation | MIR/LLVM | pass descriptor fields as arguments |
| CSE, LICM, GVN, SCCP, inlining | LLVM | classical, LLVM does it better |
| Vectorisation, unrolling, scheduling | LLVM | ditto |
| Register allocation, ISel | LLVM | ditto |

## 3. HIR passes

Run in this order; each is a fixpoint over the procedure unless noted.

1. **Attribute/constant propagation.** Fold constant expressions with exact
   PL/I precision rules (a folded `FIXED DECIMAL` keeps its scale). Propagate
   `INITIAL` values of `STATIC` non-assigned variables.
2. **Conversion folding.** Collapse chains: `CHAR→FIXED DEC→FIXED BIN` becomes
   a single `CHAR→FIXED BIN` conversion; delete conversions to the same type;
   push conversions towards constants (`X + 1` where `X` is `FIXED BIN(31)` and
   `1` is `FIXED DEC(1)` converts the *constant*, not the variable).
   This is the single highest-value PL/I-specific pass: naive conversion
   insertion is what made old PL/I code slow.
3. **Aggregate expression scalarisation and fusion.** `A = B + C * 2;` over
   conformable arrays becomes one loop nest, not three with temporaries.
   Structure expressions expand field-wise; `BY NAME` matches on names.
   Emits `pli.aggregate.loop` HIR nodes that MIR lowers.
4. **Iteration normalisation.** Canonicalise `DO` specifications: hoist `TO`/
   `BY` evaluation (the spec evaluates them once), turn a constant-sign `BY`
   into a known direction (removing the runtime sign test that M0 emits),
   convert counted loops into a trip-count form for LLVM.
5. **String length propagation.** Track current lengths of `VARYING` values
   symbolically so that assignments, comparisons and `SUBSTR` can use constants
   and so that `LENGTH(x)` folds.
6. **Picture specialisation.** Replace generic `pli_pic_edit` calls with
   specialised routines for monomorphic pictures; fold picture validation of
   constants at compile time.
7. **Condition analysis.**
   - Propagate enable-state from prefixes to every operation.
   - Delete `ON`-units for conditions that cannot be raised in their dynamic
     scope (e.g. `ON ZERODIVIDE` around code with no division).
   - Mark blocks that establish no handlers so codegen skips the handler stack.
   - Where the only established handler is `SYSTEM`, lower `SIGNAL` directly to
     the runtime default action.
8. **Storage class refinement.** Promote `CONTROLLED` variables that are never
   multiply allocated to `AUTOMATIC`; promote `BASED` references with a single
   unambiguous locator to direct references; give `AREA`s with statically known
   allocation patterns a stack-allocated backing.
9. **`DEFINED`/`iSUB` resolution.** Rewrite defined references as base
   references with index transformations; build the alias-set graph (ADR-018).

## 4. MIR passes

1. **Bounds/range check insertion** — only where enabled. Checks are pure
   comparisons plus a call to `pli_signal`, so they are optimizable code.
2. **Check elimination and merging.** Use extent facts and dominance:
   - a check dominated by an equal-or-stronger check is removed;
   - checks on an induction variable with known trip count hoist to the
     preheader as a single range check ("check widening");
   - `STRINGRANGE` checks fold when the current length is known.
3. **Concatenation flattening.** `A || B || C || D` becomes a single result
   buffer with N `memcpy`s and one length computation, instead of a left-leaning
   tree of temporaries. (M0 already computes the result length as a sum; the
   pass removes the intermediate buffers.)
4. **In-place assignment.** `S = S || T;` and `SUBSTR(S,i,n) = T;` write into
   the target without a temporary once aliasing is proven.
5. **Aggregate copy idiom.** Whole-aggregate assignment with identical layout
   becomes `llvm.memcpy`; padding-preserving copies keep struct semantics.
6. **Descriptor scalarisation.** Split descriptors into their fields at call
   boundaries where the callee is internal and not address-taken, so LLVM sees
   plain integers (this is what makes `CHARACTER(*)` parameters cheap).
7. **Temporary lifetime narrowing.** Emit `llvm.lifetime.start/end` for
   compiler temporaries and `AUTOMATIC` aggregates to enable stack colouring.
8. **Metadata attachment** (the interface to LLVM's optimizer):
   - `!tbaa` type trees for PL/I types, so `FIXED BIN` and `CHARACTER` stores
     do not alias;
   - `!alias.scope`/`!noalias` for `DEFINED` overlays and for parameters that
     PL/I guarantees distinct;
   - `noalias`, `nonnull`, `dereferenceable`, `align` on descriptor pointers;
   - `!range` on subscript values proven in range;
   - `nsw` on arithmetic *only* where `FIXEDOVERFLOW` is enabled and therefore
     checked (otherwise wrapping is observable and `nsw` would be a lie);
   - `!llvm.loop` hints from `DO` structure (trip count, no-alias).

## 5. LLVM pipeline

Standard `-O2`/`-O3` pipeline via `PassBuilder`, plus our additions:

| Position | Pass | Purpose |
|---|---|---|
| early, pre-inline | `PLIRuntimeSpecialize` | replace generic runtime calls with typed variants (`pli_put_list_fixed` for a known `FIXED BIN(31)`), mark them `readnone`/`willreturn` where true |
| early | `PLIDescriptorSROA` | promote `{ptr,len}` descriptors and `{len,data}` varying strings into scalars ahead of ordinary SROA |
| after inlining | `PLIStringIdiom` | recognise `pli_assign_char`/`pli_concat` with constant lengths and expand to `memcpy`/`memset` so LLVM's store-to-load forwarding applies |
| after inlining | `PLIOnUnitInline` | inline small on-units into their establishing block when the handler set is statically known |
| loop opts | `PLICheckHoist` | cooperate with LICM to hoist merged range checks (facts LLVM lacks) |
| late | `PLIEHSimplify` | merge adjacent EH regions, drop empty cleanups from blocks whose handlers were eliminated |
| LTO | descriptor/parameter propagation across procedures | cross-procedure extent constants |

Also enabled: ThinLTO by default at `-O2` for multi-file programs (PL/I
external procedures are separately compiled, so cross-procedure information is
otherwise lost), and PGO (`-fprofile-generate/use`) which pays off well on
condition-heavy code because on-unit paths are cold.

## 6. Optimization levels

| Flag | Front-end behaviour | LLVM | Checks |
|---|---|---|---|
| `-O0` | no HIR/MIR passes; direct lowering | `-O0` | all enabled prefixes checked; full debug info |
| `-O1` | conversion folding, check-enable propagation | `-O1` | as written |
| `-O2` (default) | all HIR/MIR passes | `-O2` + ThinLTO | as written, merged/hoisted |
| `-O3` | + aggressive aggregate fusion, on-unit inlining | `-O3` | as written |
| `-Ofast-decimal` | permits `FIXED DECIMAL` in `i64` without overflow checks when provably in range | `-O3` | `FIXEDOVERFLOW` assumed disabled |
| `-fcheck=all` | forces every computational condition enabled regardless of prefixes | any | maximum |

`-fcheck=all` exists because the most common real-world PL/I bug class
(subscript and string-range errors in code compiled with checks off) is exactly
what a modern toolchain should be able to find on demand; it is the PL/I
analogue of a sanitizer.

## 7. What we deliberately do not optimize

- **`ABNORMAL` data** (Y33-6003 optimization attributes): values must be
  re-fetched on every reference. These become `volatile` loads; no CSE, no
  hoisting. `NORMAL` is the default and is freely optimizable.
- **`IRREDUCIBLE` entries**: not treated as pure. `REDUCIBLE` entries are
  marked `readnone`/`speculatable` so calls can be CSEd — a rare case of a 1968
  language having an explicit purity annotation, and we honour it.
- **I/O ordering**: stream and record I/O calls are never reordered or merged
  across each other; only the *formatting* is specialised.

## 8. Measuring

- `-Rpass=...`/`-Rpass-missed=...` remarks are forwarded from LLVM, plus our
  own remarks (`-Rpli-check-elim`, `-Rpli-conversion`) so a user can see which
  checks were removed and which conversions remained.
- `--print-hir`/`--print-mir` dump the intermediate forms; `-emit-llvm` shows
  the IR (M0 already supports this).
- Benchmarks: a PL/I port of scalar loops (subscript-heavy), a decimal
  arithmetic workload (payroll-style), a string workload, and condition-heavy
  code with and without prefixes. The tracked figure is time relative to
  equivalent C compiled by clang at the same `-O` level.
