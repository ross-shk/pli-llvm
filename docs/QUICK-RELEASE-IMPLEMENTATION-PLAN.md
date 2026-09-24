# PL/I 1966 quick release plan

**Status (24 Sep 2026): active.** This plan schedules remaining 1966 language
coverage. `GRAMMAR-COVERAGE.md` is the current implementation ledger; there is
no separate active `C28-COVERAGE.md` file. Recent commits have delivered parts
of several QR1 slices, so the phase rows below describe the remaining scope,
not untouched features.

This plan covers IBM C28-6571-3, *PL/I Language Specifications* (July 1966).
`IMPLEMENTATION-PLAN.md` covers compiler engineering outside language
conformance. Use the tested cases in `GRAMMAR-COVERAGE.md` as the baseline;
don't schedule or estimate work that has already landed.

The source text is
`/Users/yarro/Development/PLI/references/text/C28-6571-3_PL_I_Language_Specifications_Jul66.txt`.
Use the publication's chapter and named subsection for unique 1966 features and
the TR 25.084 rule number where the specifications overlap. Resolve unclear OCR
against the page image or semantic reference before implementation; do not
infer text from the damaged scan.

## Release policy

The two phases separate broadly useful features from less common conformance
cases. Phase 1 prioritizes features that unblock ordinary batch, business,
scientific, and text-processing programs. Phase 2 covers the remaining 1966
specification.

Every slice follows the normal test-first AST/parser/sema/IRGen/runtime workflow.
Unsupported subcases remain diagnosed. A phase is complete only when its source
matrix has compile, run, negative, conversion-boundary, and I/O round-trip tests
as applicable.

Implemented behavior is a dependency, not a quick release deliverable: it
receives no effort here and must not be reimplemented. Before starting a slice,
check `GRAMMAR-COVERAGE.md` and remove behavior that is already implemented from
the slice's remaining work.

Effort estimates from the original schedule are omitted: the delivered work
has changed the remaining scope, and the remaining effort has not been
re-estimated.

## Phase 1 - Practical release

Implement remaining work in dependency order. Numeric and I/O cases may proceed
in parallel when their prerequisites are met; descriptor-based cases depend on
the aggregate parameter representation.

| Order | Slice | Remaining work |
|---|---|---|
| QR1.1 | Aggregate parameters and expressions | Remaining descriptor and whole-aggregate cases in rules (12), (34)–(38), and (127); dynamic bounds, dynamic members, `INITIAL`, and whole-array assignment expressions already have tested support. |
| QR1.2 | Fixed-decimal conformance | Resolve precision, scale, and conversion edge cases. Scaled arithmetic, overflow checks, and decimal input/output already have tested support; see rules (16), (17), and (135)–(139). |
| QR1.3 | Dynamic storage and locators | Common `POINTER`, `BASED`, `ALLOCATE`/`FREE`, and `CONTROLLED` cases are implemented. Remaining area allocation, allocation options, dynamic based arrays, and unsupported CONTROLLED shapes are tracked under QR2.3 and rules (23), (25), (87)–(90). |
| QR1.4 | Recoverable conditions | `ERROR`, `SIZE`, `SUBSCRIPTRANGE`, `ZERODIVIDE`, and programmer-named conditions have partial support. Implement and test remaining condition kinds and actions; see rules (60)–(63), (91)–(99). |
| QR1.5 | Stream and file I/O | List, data, and common edit-directed I/O, `FILE`/`STRING`, open/close, and sequential record I/O are implemented. Remaining options and format cases are listed under rules (100)–(113). |
| QR1.6 | Source compatibility | Complete the remaining abbreviations and 48-character-set forms. Recursive `%INCLUDE` and selected preprocessor directives already work; see the auxiliary-coverage section in `GRAMMAR-COVERAGE.md`. |

**Phase 1 boundaries.** Decimal floating point, `PICTURE`, keyed/direct record
files, `AREA`, uncommon condition behavior, full compile-time replacement, and
rare attributes remain later conformance work. Some record I/O, tasking, and
`CONTROLLED` behavior already exists; consult the coverage ledger before
assuming a whole feature family is unsupported.

**Exit criterion.** Representative 1960s-style payroll, inventory, matrix,
linked-record, text-file, and report programs compile without source rewriting,
produce checked reference output, and recover from their expected data and I/O
conditions. The corpus report identifies no QR1 feature among the ten most
frequent remaining blockers.

## Phase 2 - Remaining 1966 conformance

QR2 closes every remaining language family in C28-6571-3. The order below keeps
dependencies ahead of consumers; independent rows may proceed concurrently.

| Order | Remaining 1966 scope | Source | Exit criterion |
|---|---|---|---|
| QR2.1 | remaining aggregate expression forms and operators, non-assignment cross-sections, self-defining based members, correspondence, remaining `DEFINED`/`POSITION` and multidimensional iSUB forms, `SECONDARY`, `CELL`, packed layout, and full descriptor cases | Chapters 2-4, 10 | generated aggregate shape/alias matrix passes |
| QR2.2 | precision and representation cases beyond practical binary and fixed decimal: decimal and binary floating point, remaining complex/imaginary cases, conversions, `PICTURE`, sterling data, and pictured transmission | Chapters 2-4; Appendix 2 | numeric and conversion cross-product tests match reference results |
| QR2.3 | remaining `CONTROLLED` combinations and shapes, `AREA`, allocation in areas, `ALLOCATE`/`FREE` options beyond current support, remaining pointer compatibility, and list-processing edge cases | Chapters 4, 6, 8, 10 | allocation, alias, lifetime, and self-defining-list tests pass under ASan |
| QR2.4 | computational, I/O, checkout, list-processing, programmer-named, and system-action condition cases not in QR1; non-local `GO TO`; label variables; condition built-ins | Chapters 1, 2, 6; Appendix 3 | each condition's establish, raise, resume, revert, and default-action cases pass |
| QR2.5 | stream format and remote-format cases beyond current support; remaining sequential features plus direct and keyed record I/O; buffering, access, print, backwards, exclusive, environment, and file-status behavior | Chapters 4, 7, 8 | stream and record round trips cover every file organization and option family |
| QR2.6 | remaining contextual declarations and defaults, `GENERIC`/`BUILTIN`, `USES`/`SETS`, `ABNORMAL`/`NORMAL`, `REDUCIBLE`/`IRREDUCIBLE`, parameter correspondence and descriptors, and mixed-return alternate entries | Chapters 4, 5, 10 | declaration/default and call-interface matrices pass |
| QR2.7 | Appendix 1 functions and signatures not already implemented, their missing domain and precision semantics, and elemental aggregate application where absent | Appendix 1 | generated signature, type, domain-error, and aggregate tests pass |
| QR2.8 | remaining `TASK`/`EVENT`/`PRIORITY`/`WAIT`/`DELAY` semantics, task allocation, and task-local condition state; basic asynchronous `CALL`, `WAIT`, and `DELAY` are implemented | Chapters 2, 4, 6, 8, 10 | synchronization, lifetime, condition, and race-detector tests pass |
| QR2.9 | compile-time procedures, `%DO`, `%GO TO`, replacement/include rescanning, and processor control-transfer semantics beyond the supported `%DECLARE`, `%IF`, `%ACTIVATE`, `%DEACTIVATE`, `%REPLACE`, and `%INCLUDE` forms | Chapter 9 | processor examples and nested replacement/include tests reproduce expected program text |
| QR2.10 | remaining character-set, collation, abbreviation, statement, pseudo-variable, initialization, file, and semantic edge cases not closed above | Chapters 1-10; Appendices 4-6 | chapter checklist has no unsupported entry |

**Exit criterion.** A chapter-and-subsection coverage ledger for C28-6571-3 has
no planned or silently accepted entries: each item is implemented and tested, or
is recorded as an implementation-defined choice with a test and user-visible
documentation. All TR 25.084 overlap remains green.

## Delivery gates

1. Add a C28-6571-3 chapter/subsection ledger before QR1.1 and map overlapping
   entries to `GRAMMAR-COVERAGE.md`; do not duplicate specification prose.
2. Land one runnable vertical slice at a time. Parser-only acceptance is not
   progress, and unimplemented alternatives retain cited diagnostics.
3. Run `make test` for each slice and `make check` at every QR1/QR2 boundary.
   Inspect emitted LLVM IR for aggregate, decimal, storage, condition, and call
   lowering.
4. Run ASan/UBSan for storage work, race detection for tasking, and generated
   matrix tests for conversions, descriptors, formats, and built-ins.
5. Re-rank only within QR1 using measured corpus blockers. Moving a QR2 feature
   into QR1 requires displacing a lower-impact QR1 slice so the Pareto release
   does not expand without bound.

Optimization, debug information, ThinLTO, and C FFI are not 1966 language
coverage and remain in the general implementation plan.
