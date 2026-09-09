# Implementation plan

M0 and M1 are complete. The remaining milestones are ordered by utility in real
programs, not by grammar order: arrays and structures, dynamic storage,
conditions, and I/O come next. Specialized numeric and presentation features
remain diagnosed until the practical core is usable.

Effort is in engineer-weeks for one experienced compiler engineer; parallelism
notes say what can proceed concurrently.

| # | Milestone | Effort | Exit criterion |
|---|---|---|---|
| M0 | Wireframe (**done**) | - | basic scalar programs compile, link, and run |
| M1 | Procedures and control flow (**done**) | - | recursive functions, nested scopes, entries, local `GO TO` |
| M2 | Arrays and structures | 8w | matrix and record-processing programs run |
| M3 | Dynamic storage and locators | 5w | linked lists and arena allocation run |
| M4 | Conditions and non-local control | 7w | handlers, checked operations, non-local `GO TO` |
| M5 | Practical stream and file I/O | 7w | common input, output, formatting, and files work |
| M6 | Record I/O | 6w | sequential, direct, and keyed record programs run |
| M7 | Optimization and debug info | 8w | useful `-O2` performance and DWARF support |
| M8 | Preprocessor, source compatibility, and FFI | 4w | common existing sources and C interfaces compile |
| M9 | Tasking | 4w | `TASK`/`EVENT`/`WAIT` work with conditions and storage |
| D1 | Specialized scalar conformance (**deferred**) | 10w+ | decimal, complex, `PICTURE`, and full conversion lattice |
| - | Conformance hardening | ongoing | rule matrix green for completed milestones |

With M0 and M1 complete, the remaining practical core through M6 is about 33
engineer-weeks. The remaining work through M9 is about 49 engineer-weeks, plus
ongoing conformance work. D1 is intentionally outside that critical path.

## Prioritization policy

Work enters an earlier milestone when it unlocks many ordinary programs or is
a dependency of several later features. Within each milestone, implement the
smallest generally useful slice first and diagnose the remainder with its rule
number.

M0 and M1 provide the scalar, procedure, and control-flow foundation. Remaining
work is prioritized as follows:

1. Arrays, structures, subscripting, and aggregate assignment.
2. Dynamic storage, pointers, locators, and allocation lifetimes.
3. Conditions and non-local control needed for robust programs.
4. Common stream and record I/O.
5. Tooling, optimization, compatibility, and concurrency.
6. Specialized representations and exhaustive conversion conformance.

Feature rarity alone is not decisive: a deferred feature may move forward when
the compile-and-run corpus shows that it blocks substantially more programs
than the next planned item.

---

## M1 - Procedures and control flow (done)

**Delivered**

- `llvm::IRBuilder` behind the existing `IRGen` interface (ADR-002), with
  `--emit-textual-ir` retained for tests.
- HIR (ADR-005) between sema and codegen, initially as a thin AST mirror, plus
  `--print-hir`.
- `RETURNS`, function references, `RECURSIVE`, multiple entry points, and
  internal procedures accessing enclosing automatic storage.
- Real lexical scopes for `BEGIN` blocks and local `GO TO`.
- Speculative statement-keyword disambiguation (ADR-004 step 3).
- IR golden tests, diagnostic fix-its, and `--explain <rule>`.

Recursive functions, nested procedures, multiple entries, ordinary structured
control flow, local `GO TO`, LLVM-backed IR generation, and the supporting
diagnostic and IR tests are implemented.

---

## M2 - Arrays and structures (8w)

**Scope**

- Arrays with constant and dynamic bounds, `*` extents, cross-sections, and
  row-major layout; use dope vectors where required (ADR-008).
- Structure level numbers, factoring, `LIKE`, nested arrays of structures, and
  `ALIGNED`/`UNALIGNED` mapping.
- Subscripted and qualified references, aggregate expressions and assignment,
  multiple assignment, and `BY NAME`.
- Full procedure descriptors needed to pass arrays and structures safely.
- `INITIAL` iteration factors and `INITIAL CALL`.
- Common array built-ins: `SUM`, `PROD`, `ANY`, `ALL`, `DIM`, `LBOUND`, and
  `HBOUND`; `SUBSCRIPTRANGE` checking.
- `DEFINED`/`iSUB` and string overlays after ordinary arrays and structures are
  complete, because they add aliasing complexity without unlocking the core.

**Exit criterion.** Matrix, table-processing, and nested-record programs pass
arrays and structures between procedures and run with bounds checking enabled.

---

## M3 - Dynamic storage and locators (5w)

**Scope**

- `BASED`, locator qualification (`P->X`), and `POINTER`.
- `ALLOCATE`/`FREE` (rules 87-90) and `CONTROLLED` generation stacks.
- `AREA`/`OFFSET` with a runtime sub-allocator.
- `ADDR`, `NULL`, `NULLO`, `EMPTY`, and `ALLOCATION`.
- `AREA` condition integration required for safe allocation failures.
- Self-defining structures only after standard locator semantics are complete.

**Exit criterion.** Linked-list, tree, and arena-allocation programs run;
allocation lifetimes are covered by ASan tests and `AREA` assignment preserves
offsets.

---

## M4 - Conditions and non-local control (7w)

**Scope** `ON`/`REVERT`/`SIGNAL` (rules 91-99), handler stacks, on-units with
access to the establishing frame, `SNAP`, `SYSTEM`, non-local `GO TO`, `LABEL`
variables carrying frames, condition-prefix enable state, computational
conditions, `ERROR`/`FINISH`, and `CHECK`/`NOCHECK`.

Implement conditions needed by earlier milestones first:
`SUBSCRIPTRANGE`, allocation/`AREA`, arithmetic faults, and `ERROR`. Add I/O
conditions with M5 and M6 rather than blocking the core handler mechanism.

**Exit criterion.** Checked array and allocation failures can be handled and
resumed as specified; non-local `GO TO` out of an on-unit works; code with no
active handlers has measured near-zero overhead.

**Risk.** LLVM EH, non-local control, and optimization interact. Start with an
early `invoke`/`landingpad` and `pli_goto_nonlocal` spike.

---

## M5 - Practical stream and file I/O (7w)

**Scope**

- `OPEN`/`CLOSE` with common attributes and options (rules 100-103).
- `GET`/`PUT` with `FILE`, `STRING`, `SKIP`, `PAGE`, `LINE`, and `COPY`.
- List- and data-directed transmission first, then commonly used
  edit-directed items and `FORMAT` statements (rules 44-55).
- `SYSIN`/`SYSPRINT`, `LINESIZE`/`PAGESIZE`, and relevant I/O conditions.
- Formatting of supported binary, character, and bit values. `PICTURE` data
  and sterling formats remain in D1.

**Exit criterion.** Command-line, text-processing, and ordinary report programs
read and write files with stable column and page behavior.

---

## M6 - Record I/O (6w)

**Scope** `READ`/`WRITE`/`REWRITE`/`DELETE`/`LOCATE`/`UNLOCK` (rules 112-113),
sequential/direct/keyed organization, common option sets, record conditions,
and an indexed-file implementation or bridge to an external one.

**Note.** The scan of rule (113) is incomplete. Implement the option set from
Y33-6003 and record the divergence.

**Exit criterion.** Sequential, direct, and keyed applications persist and
update array- and structure-backed records correctly.

---

## M7 - Optimization and debug info (8w)

**Scope** MIR; the HIR/MIR and custom LLVM passes in OPTIMIZATION.md; alias
metadata; ThinLTO; DWARF descriptions for structures, varying strings, arrays,
and dope vectors; optimization remarks; and the benchmark suite.

**Exit criterion.** Scalar and array benchmarks are within 1.2x of equivalent C
at `-O2`; check-heavy code is within 1.5x of check-free code; common data types
are inspectable at `-O0` and usefully inspectable at `-O2`.

---

## M8 - Preprocessor, source compatibility, and FFI (4w)

**Scope** The `%` preprocessor (`%INCLUDE`, `%DECLARE`, `%IF`, `%DO`,
`%PROCEDURE`); source character-set compatibility and keyword abbreviations;
dialect switches; `OPTIONS(BYVALUE)` and C interoperability attributes.

Prioritize compatibility features by measured corpus impact. Do not copy
later-dialect syntax from the reference corpus into the TR 25.084 language.

**Exit criterion.** Representative in-scope existing sources preprocess and
compile, and supported C interfaces work in both directions.

---

## M9 - Tasking (4w)

**Scope** `TASK`/`EVENT`/`PRIORITY`/`WAIT` on threads (ADR-016), thread-local
condition-handler and controlled-generation stacks, and `ABNORMAL` visibility.

**Exit criterion.** Task creation, synchronization, condition handling, and
controlled allocation pass race-detector-backed tests.

---

## D1 - Specialized scalar conformance (deferred, 10w+)

This work remains explicitly diagnosed rather than partially accepted. It can
proceed independently once M3 stabilizes, but it does not delay the practical
core.

**Scope**

- Full `FIXED DECIMAL(p,q)` precision and scale semantics, decimal floating
  point, packed representations, and decimal performance work.
- `COMPLEX` arithmetic and imaginary constants.
- `PICTURE`, sterling pictures/constants, numeric-character data, and pictured
  input/output.
- The exhaustive arithmetic/character/bit/picture conversion lattice and
  `CONVERSION` edge cases.
- Less common arithmetic built-ins and precision variants not required by the
  earlier milestones.

**Exit criterion.** Payroll-style decimal and pictured-output programs produce
byte-identical reference results; generated conversion cross-product tests
pass.

## Cross-cutting workstreams

**Conformance.** GRAMMAR-COVERAGE.md remains the rule ledger. Definition of
done for a milestone includes moving its rules to implemented and tested.
Deferred rules stay marked D1 and must produce rule-numbered diagnostics.

**Corpus.** Continue running the in-scope compile-and-run corpus established in
M1. Track both programs compiling and the unsupported features that block the
most programs; use those counts to adjust priority at milestone boundaries.

**Continuous integration.** Build on Linux/macOS with clang/gcc; run unit,
golden, execution, diagnostic, corpus, ASan/UBSan, and nightly grammar-fuzzing
jobs.

**Documentation.** Each milestone updates architecture and grammar coverage,
adds ADRs only for decisions taken, and records user-visible deviations once.

## Risk register

| Risk | Impact | Mitigation |
|---|---|---|
| Aggregate descriptors spread across the ABI | correctness | stabilize array/structure descriptors before storage and I/O depend on them |
| Dynamic storage introduces lifetime bugs | correctness | ASan tests for every allocation mode and controlled generation |
| EH and non-local control interact with optimization | correctness | early lowering spike and an optimization fallback |
| I/O scope expands into rare formatting | schedule | deliver list/data-directed I/O first; keep `PICTURE` in D1 |
| Corpus pressure pulls in later-dialect syntax | scope | require a TR 25.084 rule before changing language acceptance |
| Deferred features become silently accepted | conformance | retain rule-numbered diagnostics and D1 ledger entries |
| Spec gaps from OCR damage | conformance | resolve against semantic references and record each divergence |
