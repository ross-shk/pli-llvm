# C-mirror quick release sub-plan

This is a small, optional path through
`QUICK-RELEASE-IMPLEMENTATION-PLAN.md`. It selects only remaining PL/I 1966
features with a close standard C language or library analogue. PL/I syntax and
semantics remain authoritative; C is used only to choose implementation order
and to reuse proven LLVM/runtime techniques.

The baseline is the current implemented-and-tested state in
`GRAMMAR-COVERAGE.md`. Existing scalar control flow, procedures, fixed and
single-axis dynamic arrays, structures, strings, binary arithmetic, fixed
decimal arithmetic, `PUT SKIP/PAGE LIST`, and listed built-ins are excluded.
Before starting an item, remove any feature that has since become implemented
and reduce its estimate.

## Scope (11 engineer-weeks)

Implement in order; CM3 and CM4 may proceed in parallel after CM1 fixes the
remaining call descriptors.

| Order | Parent | Remaining feature | C analogue | Effort |
|---|---|---|---|---:|
| CM1 | QR1.1 | dynamic lower and multi-axis bounds, runtime aggregate lengths, remaining descriptors, structure expression values, and structure initialization | variable-length arrays, array parameters, structures by value, aggregate initialization | 2w |
| CM2 | QR1.3 | `POINTER`, based data, `->`, `ADDR`, `NULL`, and common `ALLOCATE`/`FREE` forms | pointers, address-of, null pointers, `malloc`/`free` | 2w |
| CM3 | QR1.5 | `OPEN`/`CLOSE`, list-directed input, file/string sources and sinks, and common edit-directed numeric and character items beyond existing output | streams, `fopen`/`fclose`, `scanf`/`printf`, string formatting | 3w |
| CM4 | QR1.6, QR2.9 | safe `%INCLUDE`, object-like replacement, activation needed for replacement, and conditional compilation | `#include`, object-like macros, `#if` | 2w |
| CM5 | QR2.2, QR2.7 | complex representation and arithmetic, missing real/complex conversions, and unimplemented Appendix 1 functions corresponding to C `<math.h>` or `<complex.h>` | C complex types and standard mathematical functions | 2w |

CM5 excludes built-ins already recorded in `GRAMMAR-COVERAGE.md`, including
`ABS`, `MIN`, `MAX`, `MOD`, `TRUNC`, `ROUND`, `INDEX`, `SUBSTR`, `LENGTH`,
`REPEAT`, `DATE`, `TIME`, and the implemented array inquiries and reductions.
Start with the remaining C counterparts such as `FLOOR`, `CEIL`, `SQRT`, `EXP`,
the logarithmic and trigonometric families, and complex component/conjugate
operations.

## Explicit exclusions

- Fixed decimal arithmetic already implemented is not work. Remaining decimal
  precision, overflow, and decimal I/O stay in the parent plan because standard
  C has no fixed-decimal type.
- PL/I conditions and non-local control are not C exception facilities.
- `AREA`, controlled-generation stacks, list-processing edge cases, keyed and
  record I/O, `PICTURE`, sterling data, and PL/I file organization have no close
  standard C language analogue.
- PL/I task/event semantics and compile-time `%DO`, `%GO TO`, rescanning control
  transfers, and compile-time procedures exceed the standard C analogue.
- C FFI, ABI compatibility, optimization, and debug information remain in the
  general implementation plan and are not PL/I language features.

## Exit criterion

A PL/I test set equivalent to small C programs using variable-length aggregate
interfaces, linked heap records, text streams, conditional includes, complex
arithmetic, and standard mathematics compiles and produces matching results.
Each item has negative and boundary tests, emitted LLVM IR is inspected, storage
tests pass under ASan/UBSan, and `make test` plus `make check` are green.

Completing this sub-plan closes only its mapped QR gaps. It does not change the
QR1 or QR2 completion criteria.
