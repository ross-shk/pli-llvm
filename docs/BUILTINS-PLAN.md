# Built-in functions plan (implementation track)

Every PL/I built-in lives on this track — not scattered across
MODERN-PLI-PLAN (MX5) or the QR milestones. Each builtin is served
by the same four-layer chain, tested in `tests/builtins/`, and
guarded by the AGENTS.md builtins silo (other work stays out).

## Pipeline

`sema typeBuiltin` (typing + arity/shape checks) → HIR passthrough
(no per-builtin lowering) → `irgen emitBuiltin` (codegen; inline
where trivial, else a `pli_*` runtime call) → `runtime/rt_*.c`
via `runtime/pli_rt_abi.def`, the single source of truth for the
`pli_*` ABI. New builtins follow the extension vehicle: ADR +
test-first (`tests/builtins/`) + diagnostics for unserved forms.

## Status: string (rule 123, Appendix 1)

| Builtin | Test |
|---|---|
| `SUBSTR` (+ assignment target) | `substr.pli`, `substr_assign.pli`, `bad_substr.pli`, `bad_substr_assign.pli` |
| `INDEX`, `LENGTH` | `index.pli`, `length.pli`, `bad_index.pli`, `bad_length.pli` |
| `REPEAT`, `VERIFY`, `TRANSLATE` | `repeat.pli`, `verify.pli`, `translate.pli`, `bad_repeat.pli`, `bad_verify.pli`, `bad_translate.pli` |
| `TRIM`, `TALLY` (ADR-107) | `trim.pli`, `tally.pli`, `bad_trim.pli`, `bad_tally.pli` |
| `HIGH`, `LOW` | `highlow.pli`, `bad_highlow.pli` |

## Status: math (rule 123, Appendix 1)

| Builtin | Test |
|---|---|
| `ABS`, `ROUND`, `TRUNC`, `PRECISION`, `MIN`, `MAX`, `MOD`, `MULTIPLY`, `DIVIDE` | `abs.pli`, `round.pli`, `min.pli`, `max.pli`, `bad_abs.pli`, `bad_round.pli`, `bad_min.pli`, `bad_max.pli`, `math4.pli`, `bad_math4.pli` |
| `FLOOR`/`CEIL`/`SQRT`/`EXP`/`LOG`/`SIN`/`COS`/`TAN`/`LOG2`/`LOG10`/`ATAN`/`SINH`/`COSH`/`TANH`/`ATANH`/`ERF`/`ERFC` + degree variants + `ASIN`/`ACOS`/`ATAN2`/`CBRT` (ADR-072) | `math.pli`, `math2.pli`, `math3.pli`, `bad_math.pli`, `bad_math2.pli`, `bad_math3.pli` |
| `COMPLEX`/`REAL`/`IMAG`/`CONJG` (ADR-073/074) | `complex.pli`, `complex_var.pli`, `bad_complex.pli`, `bad_complex_var.pli` |

## Status: array, pointer, misc

| Builtin | Test |
|---|---|
| `LBOUND`/`HBOUND`/`DIM`, `SUM`/`PROD`/`ANY`/`ALL` | `array_bounds.pli`, `array_reduce.pli`, `array_param.pli`, `bad_array_builtin.pli`, `bad_array_reduce.pli` |
| `NULL`, `ADDR` (ADR-063) | `pointer.pli` |
| `DATE`, `TIME` | `datetime.pli`, `bad_datetime.pli` |
| `OMITTED`, `PRESENT` (ADR-119) | `optional.pli`, `bad_omitted.pli` (stay in `core`: an OPTIONAL-feature test, same judgment as `array_param.pli`) |
| `ONCODE` | exercised via `on_error.pli` (stays in `core`: a condition test, not a builtins test) |

## Milestones (unserved remainder, one slice each)

| Slice | Scope |
|---|---|
| BX-STRING | `COMPARE`, `MEMCONVERT`, `CENTER`/`LEFT`/`RIGHT` if admitted — each needs its Enterprise-semantics note first |
| BX-MATH | Remaining Appendix-1 gaps; precision establishment beyond current coverage |
| BX-ARRAY | `POLY`, `PROD` overflows, `ALL`/`ANY` on non-BIT; cross-section builtins with rule-126 values |

Each slice: verify Enterprise semantics, ADR, `tests/builtins/` first, diagnostics for the rest. None else is approved by this document. (`LTRIM`/`RTRIM` are not Enterprise built-ins — dropped, ADR-107.)

## Exit criterion

Every served builtin above has a row, an ADR where it decided
something, and a `tests/builtins/` test; `make test` is green; the
Extensions ledger points here for builtin extensions.
