# PL/I 1966 quick release plan

This supplement schedules coverage of IBM C28-6571-3, *PL/I Language
Specifications* (July 1966). `IMPLEMENTATION-PLAN.md` remains the plan for
compiler engineering work that is not part of language coverage. The current
implemented-and-tested entries in `GRAMMAR-COVERAGE.md`, including work completed
after M1, are the baseline. Transferred language work is owned here and is
marked `QR1` or `QR2` in the general plan.

The source text is
`/Users/yarro/Development/PLI/references/text/C28-6571-3_PL_I_Language_Specifications_Jul66.txt`.
Use the publication's chapter and named subsection for unique 1966 features and
the TR 25.084 rule number where the specifications overlap. Resolve unclear OCR
against the page image or semantic reference before implementation; do not
infer text from the damaged scan.

## Release policy

The Pareto split is by independently useful feature slices, not by pages or
grammar productions. Phase 1 deliberately implements about 20% of the remaining
feature surface that enables the broadest class of batch, business, scientific,
and text-processing programs. Phase 2 is the conformance pass for everything
else in the 1966 publication.

Every slice follows the normal test-first AST/parser/sema/IRGen/runtime workflow.
Unsupported subcases remain diagnosed. A phase is complete only when its source
matrix has compile, run, negative, conversion-boundary, and I/O round-trip tests
as applicable.

Implemented behavior is a dependency, not a quick release deliverable: it
receives no effort here and must not be reimplemented. Before starting a slice,
remove any gaps that have since moved to implemented-and-tested in
`GRAMMAR-COVERAGE.md` and reduce the estimate accordingly.

## Phase 1 - Pareto release (18 engineer-weeks)

Implement these slices in dependency order. Work on decimal arithmetic and the
stream runtime may proceed in parallel after aggregate descriptors stabilize.

| Order | Slice | Effort | Deliverable |
|---|---|---:|---|
| QR1.1 | Remaining practical aggregate gaps | 3w | dynamic lower and multi-axis bounds, dynamic structure members and lengths, general dope-vector descriptors beyond current extent passing, whole aggregate expression values, and `INITIAL` for dynamic arrays and structures |
| QR1.2 | Commercial fixed decimal | 4w | `FIXED DECIMAL(p,q)` storage, constants, arithmetic, comparison, assignment conversion, rounding, and decimal `GET`/`PUT`; overflow and conversion checks included |
| QR1.3 | Practical dynamic records | 2w | `POINTER`, based structures, `->`, `ADDR`, `NULL`, and common `ALLOCATE`/`FREE` forms for linked records |
| QR1.4 | Recoverable conditions | 3w | enforce already-parsed condition prefixes and add `ON`, `REVERT`, and `SIGNAL` for `ERROR`, arithmetic, conversion, subscript, allocation, and common I/O conditions |
| QR1.5 | Remaining practical stream and file I/O | 4w | `OPEN`/`CLOSE`; input; `FILE`, `STRING`, `LINE`, and `COPY` options; data-directed transmission; and common edit-directed numeric, character, spacing, line, and page items beyond existing `PUT SKIP/PAGE LIST` |
| QR1.6 | Remaining high-impact source compatibility | 2w | 1966 keyword abbreviations and 48-character forms not already accepted, plus safe `%INCLUDE` without the remaining compile-time processor |

**Phase 1 boundaries.** Decimal floating point, `COMPLEX`, `PICTURE`, record
I/O, keyed files, tasking, areas, controlled-generation stacks, full compile-time
replacement, and rare attributes stay diagnosed for QR2. QR1 formatting covers
ordinary reports but not pictured or sterling data.

**Exit criterion.** Representative 1960s-style payroll, inventory, matrix,
linked-record, text-file, and report programs compile without source rewriting,
produce checked reference output, and recover from their expected data and I/O
conditions. The corpus report identifies no QR1 feature among the ten most
frequent remaining blockers.

## Phase 2 - Complete 1966 coverage (45+ engineer-weeks)

QR2 closes every remaining language family in C28-6571-3. The order below keeps
dependencies ahead of consumers; independent rows may proceed concurrently.

| Order | Remaining 1966 scope | Source | Exit criterion |
|---|---|---|---|
| QR2.1 | general aggregate expression use and operators, non-assignment cross-sections, self-defining based members, correspondence, remaining `DEFINED`/`POSITION` and multidimensional iSUB forms, qualified `LIKE`, `SECONDARY`, `CELL`, packed layout, and descriptor cases not closed by QR1 | Chapters 2-4, 10 | generated aggregate shape/alias matrix passes |
| QR2.2 | precision and representation cases beyond practical binary and QR1 fixed decimal: decimal and binary floating point, complex and imaginary data, bit strings longer than one bit, remaining conversions, `PICTURE`, sterling data, and pictured transmission | Chapters 2-4; Appendix 2 | numeric and conversion cross-product tests match reference results |
| QR2.3 | `CONTROLLED` generation stacks, `AREA`, allocation in areas, `ALLOCATE`/`FREE` options beyond QR1, remaining pointer compatibility, and list-processing edge cases | Chapters 4, 6, 8, 10 | allocation, alias, lifetime, and self-defining-list tests pass under ASan |
| QR2.4 | computational, I/O, checkout, list-processing, programmer-named, and system-action condition cases not in QR1; non-local `GO TO`; label variables; condition built-ins | Chapters 1, 2, 6; Appendix 3 | each condition's establish, raise, resume, revert, and default-action cases pass |
| QR2.5 | stream format and remote-format cases beyond QR1; sequential, direct, and keyed record I/O; buffering, access, print, backwards, exclusive, environment, and file-status behavior | Chapters 4, 7, 8 | stream and record round trips cover every file organization and option family |
| QR2.6 | contextual and implicit declarations, defaults, `GENERIC`/`BUILTIN`, `USES`/`SETS`, `ABNORMAL`/`NORMAL`, `REDUCIBLE`/`IRREDUCIBLE`, remaining parameter correspondence and descriptors, and mixed-return alternate entries | Chapters 4, 5, 10 | declaration/default and call-interface matrices pass |
| QR2.7 | Appendix 1 functions and signatures not already implemented, their missing domain and precision semantics, and elemental aggregate application where absent | Appendix 1 | generated signature, type, domain-error, and aggregate tests pass |
| QR2.8 | `TASK`, `EVENT`, `PRIORITY`, `WAIT`, `DELAY`, asynchronous calls, task allocation, and task-local condition state | Chapters 2, 4, 6, 8, 10 | synchronization, lifetime, condition, and race-detector tests pass |
| QR2.9 | compile-time declarations, expressions, activation, replacement and rescanning, `%IF`, `%DO`, `%GO TO`, full `%INCLUDE` rescanning and control-transfer semantics beyond QR1, and compile-time procedures | Chapter 9 | processor examples and nested replacement/include tests reproduce expected program text |
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
