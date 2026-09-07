# Implementation plan

Milestones are ordered so that each one ends with a compiler that runs real
programs. Effort is in engineer-weeks for one experienced compiler engineer;
parallelism notes say what can proceed concurrently.

| # | Milestone | Effort | Exit criterion |
|---|---|---|---|
| M0 | Wireframe (**done**) | — | `hello.pli` + 7 tests compile, link and run |
| M1 | LLVM API, procedures, functions | 6w | recursive functions, nested scopes, IR golden tests |
| M2 | Full scalar type system | 10w | decimal arithmetic, `PICTURE`, conversions, bit strings |
| M3 | Aggregates | 8w | arrays, structures, `LIKE`, `DEFINED`/`iSUB`, aggregate expressions |
| M4 | Storage classes and locators | 5w | `BASED`, `CONTROLLED`, `AREA`/`OFFSET`, `ALLOCATE`/`FREE` |
| M5 | Conditions | 7w | `ON`/`REVERT`/`SIGNAL`, prefixes, non-local `GO TO` |
| M6 | Stream I/O | 7w | `GET`/`PUT` list/data/edit-directed, `FORMAT`, files |
| M7 | Record I/O | 6w | sequential/direct/keyed, `OPEN`/`CLOSE`, I/O conditions |
| M8 | Optimization and debug info | 8w | HIR/MIR passes, custom LLVM passes, DWARF |
| M9 | Tasking, preprocessor, compatibility | 8w | `TASK`/`EVENT`/`WAIT`, `%INCLUDE`, EBCDIC, 48-char |
| — | Conformance hardening | ongoing | full rule matrix green, corpus compiles |

Total to a production-credible compiler: ~65 engineer-weeks, plus ongoing
conformance work. Two engineers can run M2/M3 and M5/M6 in parallel after M1.

---

## M1 — LLVM C++ API, procedures and functions (6w)

**Scope**

- Replace the textual emitter with `llvm::IRBuilder` behind the existing
  `IRGen` interface (ADR-002); keep `--emit-textual-ir` for tests.
- CMake build with `find_package(LLVM)`; keep the Makefile for the bootstrap.
- Introduce HIR (ADR-005) between sema and codegen, initially a thin mirror of
  the AST, plus `--print-hir`.
- Procedures: `RETURNS` and function references, `RECURSIVE`, multiple entry
  points (`ENTRY` statement, rule 56), static links/displays for internal
  procedures accessing enclosing automatic storage (removes the M0 static
  storage deviation in ADR-010).
- `BEGIN` blocks as real scopes; `GO TO` within a procedure; `LEAVE`-style exits
  via `GO TO`.
- Speculative-parse keyword disambiguation (ADR-004 step 3).
- Diagnostics: fix-its, `--explain <rule>`.

**Exit criterion.** A recursive `FACTORIAL` function and a two-entry-point
procedure compile and run; IR golden tests in place; all M0 tests still pass.

**Risks.** LLVM version churn — pin a version and use the C++ API only through
our wrapper.

---

## M2 — Full scalar type system (10w)

**Scope**

- `FIXED DECIMAL(p,q)`/`FIXED BINARY(p,q)` with exact precision/scale rules
  (ADR-006), `i64`/`i128`/BCD tiers, `SIZE`/`FIXEDOVERFLOW` detection.
- Correct `/` and `**` (removes the ADR-014 deviation).
- `COMPLEX` arithmetic; `FLOAT BINARY/DECIMAL` precision tiers.
- Bit strings of arbitrary length: packed representation, `&`/`|`/`¬`,
  comparison, `SUBSTR`/`BOOL`.
- The full conversion lattice (arithmetic ↔ character ↔ bit ↔ picture) with
  `CONVERSION` condition support.
- `PICTURE`: parse (rules 146–148), field program, edit/validate, sterling
  pictures, numeric-character data as an arithmetic type.
- Built-in functions: arithmetic (`ABS`, `MOD`, `ROUND`, `TRUNC`, `MAX`, `MIN`,
  `DIVIDE`, `MULTIPLY`, `PRECISION`), mathematical, string (`SUBSTR`, `INDEX`,
  `LENGTH`, `TRANSLATE`, `VERIFY`, `REPEAT`, `HIGH`, `LOW`), `DATE`/`TIME`.
- `SUBSTR` as a pseudo-variable (assignment target).

**Exit criterion.** A payroll-style program using `FIXED DECIMAL(11,2)` and
pictured output produces byte-identical results to a reference implementation;
conversion conformance tests pass.

**Risks.** The conversion rules are the largest single body of PL/I semantics;
budget generously and test exhaustively (generated cross-product tests).

---

## M3 — Aggregates (8w)

**Scope**

- Arrays: bounds (constant and dynamic), `*` extents, cross-sections (`A(*,3)`),
  row-major layout, dope vectors where required (ADR-008).
- Structures: level numbers, `LIKE`, nested arrays of structures, the mapping
  rules with `ALIGNED`/`UNALIGNED`.
- Aggregate expressions and assignment, `BY NAME`.
- `DEFINED`/`iSUB` defining and string overlay defining (ADR-018), including
  rule (134) `isub`.
- `INITIAL` with iteration factors and `INITIAL CALL`.
- Array built-ins (`SUM`, `PROD`, `ANY`, `ALL`, `DIM`, `LBOUND`, `HBOUND`).
- `SUBSCRIPTRANGE` checking.

**Exit criterion.** Matrix and record-processing programs from
`references/code/**` compile and run correctly.

---

## M4 — Storage classes and locators (5w)

**Scope** `BASED` with locator qualification (`P->X`), `POINTER`/`OFFSET`,
`AREA` with a runtime sub-allocator, `ALLOCATE`/`FREE` (rules 87–90),
`CONTROLLED` generation stacks, `ADDR`/`NULL`/`NULLO`/`EMPTY`/`ALLOCATION`,
`AREA` condition, self-defining structures (`REFER`-style, as an extension).

**Exit criterion.** A linked-list and an arena-allocation program run;
`AREA` assignment preserves offsets.

---

## M5 — Conditions (7w)

**Scope** `ON`/`REVERT`/`SIGNAL` (rules 91–99), the handler stack, on-units as
functions with access to the establishing frame, `SNAP`, `SYSTEM` action,
non-local `GO TO` with `LABEL` variables carrying frames, prefix enable-state
(rules 60–63) end to end, computational conditions, `ERROR`/`FINISH` and normal
termination, `CHECK`/`NOCHECK`, `ONCODE`-style built-ins as extensions.

**Exit criterion.** Programs relying on `ON ENDFILE`, `ON CONVERSION` retry, and
`GO TO` out of an on-unit behave per spec; zero cost when no handlers exist
(measured).

**Risks.** Highest-risk milestone: interaction of LLVM EH, non-local goto and
optimization. Mitigate with an early spike on `invoke`/`landingpad` +
`pli_goto_nonlocal`.

---

## M6 — Stream I/O (7w)

**Scope** `OPEN`/`CLOSE` with attributes and options (rules 100–103),
`GET`/`PUT` with `FILE`/`STRING`/`SKIP`/`PAGE`/`LINE`/`COPY`, list-, data- and
edit-directed transmission, `FORMAT` statements and remote formats (rules
44–55), the format engine, `SYSIN`/`SYSPRINT`, `LINESIZE`/`PAGESIZE`,
`ENDPAGE`/`ENDFILE`/`NAME`/`CONVERSION` conditions.

**Exit criterion.** Report-generating programs produce byte-identical output to
a reference implementation, including column/page behaviour (this also removes
the M0 `SKIP` deviation documented in `runtime/pli_rt.c`).

---

## M7 — Record I/O (6w)

**Scope** `READ`/`WRITE`/`REWRITE`/`DELETE`/`LOCATE`/`UNLOCK` (rules 112–113),
`SEQUENTIAL`/`DIRECT`/`KEYED`/`BUFFERED`/`EXCLUSIVE`, `INTO`/`FROM`/`SET`/
`KEY`/`KEYFROM`/`KEYTO`/`IGNORE`/`NOLOCK`/`EVENT`, `ENVIRONMENT` options, record
formats, `KEY`/`RECORD`/`TRANSMIT`/`UNDEFINEDFILE` conditions, an indexed file
implementation (B-tree) or a bridge to an external one.

**Note.** The scan of TR 25.084 rule (113) is incomplete (see the ⚠ note in the
grammar extraction); implement the option set from Y33-6003 and record the
divergence.

---

## M8 — Optimization and debug info (8w)

**Scope** MIR; every HIR/MIR pass in OPTIMIZATION.md §3–4; the custom LLVM
passes in §5; TBAA/alias metadata; ThinLTO; DWARF debug info with PL/I type
descriptions (structures, varying strings, arrays with dope vectors) so `lldb`
can print PL/I variables; optimization remarks; the benchmark suite in §8.

**Exit criterion.** Scalar/array benchmarks within 1.2× of equivalent C at
`-O2`; check-heavy code within 1.5× of check-free; debuggable at `-O0` and
usefully debuggable at `-O2`.

---

## M9 — Tasking, preprocessor, compatibility (8w)

**Scope** `TASK`/`EVENT`/`PRIORITY`/`WAIT` on threads (ADR-016), thread-local
handler and generation stacks, `ABNORMAL` as volatile; the `%` preprocessor
(`%INCLUDE`, `%DECLARE`, `%IF`, `%DO`, `%PROCEDURE`); EBCDIC source, card
margins, 48-character set (ADR-013); IBM dialect switches; `OPTIONS(BYVALUE)`
and C interop attributes for FFI.

---

## Cross-cutting workstreams

**Conformance.** GRAMMAR-COVERAGE.md is the ledger: every rule (1)–(151) has an
owner milestone and at least one test. Definition of done for a milestone
includes flipping its rules to "implemented + tested".

**Corpus.** `references/code/**` (Iron Spring samples, MULTICS sources,
RosettaCode tasks, course code) becomes a compile-and-run corpus from M2. Track
"programs compiling" as a headline number per milestone.

**Continuous integration.** Build on Linux/macOS × clang/gcc; run unit, golden,
execution, diagnostic and corpus suites; ASan/UBSan build of the compiler and
runtime; a nightly fuzzing job over the grammar.

**Documentation.** Each milestone updates ARCHITECTURE.md, adds ADRs for
decisions taken, and maintains a user manual mapping PL/I features to
`plic` flags and any documented deviations.

## Risk register

| Risk | Impact | Mitigation |
|---|---|---|
| Conversion rules underestimated (M2) | schedule | generated cross-product tests; start with a reference table extracted from Y33-6003 |
| EH + non-local goto + optimizer interaction (M5) | correctness | early spike; keep a `-fno-optimize-eh` fallback |
| LLVM API churn | maintenance | pin version; isolate behind `IRGen` |
| Decimal performance | adoption | ADR-006 tiering; benchmark early against packed BCD |
| Legacy dialect divergence | adoption | compatibility switches, and document every deviation in one place |
| Spec gaps from OCR damage (rules 65, 113, 115) | conformance | resolved against Y33-6003/C28-8201-1 where possible — rules (114), (148) and TR §2.3.3 are now recovered; each remaining divergence is recorded in the grammar file's ⚠ notes |
