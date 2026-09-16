# Modern PL/I niceties plan (extension track)

TR 25.084 and C28-6571-3 are 1960s language; everyday modern PL/I
(Enterprise-style `SELECT`, loop exits, an iterative preprocessor, a few
string built-ins) sits outside both. This plan adds a small set of those
niceties as a track alongside QR1/QR2, reusing the established extension
vehicle: each item gets a `GRAMMAR-COVERAGE.md` Extensions row, an ADR,
test-first coverage (golden or self-checking), and diagnostics for its
unserved subforms. The TR gate stays: anything here that collides with a
TR production follows the TR; genuinely new syntax is contextual only —
the lexer still classifies no keywords (ADR-004 pattern), so existing
uses of `SELECT`, `LEAVE`, etc. as identifiers keep working.

Prior art: `%REPLACE` (ADR-077), `%INCLUDE` paths (ADR-078),
`Stmt::Leave` as a rejected placeholder. Out of scope by prior
decision: program arguments (no argv story) and anything already
tracked (non-local `GO TO`, `OPTIONS(BYVALUE)`, `PICTURE`, areas,
tasking, keyed files).

## Phase MX-A — control flow (3.5 engineer-weeks)

| Order | Slice | Effort | Deliverable |
|---|---|---:|---|
| MX1 | `SELECT` / `WHEN` / `OTHERWISE` | 1.5w | `SELECT [(expr)]; {WHEN (pred) ...} [OTHERWISE ...] END;` lowering to the `IF`-chain in HIR; fall-through and empty-`WHEN` diagnosed, never silent |
| MX2 | `LEAVE` / `ITERATE` [label] | 1w | loop exit/continue reusing the `Leave` AST kind (new `Iterate` beside it); Innermost-loop default, labeled form resolved in sema; use outside a loop diagnosed |
| MX3 | `DO UNTIL (expr)` | 0.5w | post-test loop beside `DO WHILE`; combined `WHILE`+`UNTIL` diagnosed as a follow-up, not invented |
| MX4 | Condition-prefix enablement (rules 60–63) | 0.5w | `(NOSUBSCRIPTRANGE)` et al. elide the matching inline checks per ARCHITECTURE §5 instead of warning |

## Phase MX-B — data and built-ins (3 engineer-weeks)

| Order | Slice | Effort | Deliverable |
|---|---|---:|---|
| MX5 | Modern string built-ins | 1w | `TRIM` and `TALLY`, verified against Enterprise semantics first (`LTRIM`/`RTRIM` are not Enterprise built-ins — dropped, ADR-107); each is an independent runtime + `emitBuiltin` case with golden tests; anything beyond (e.g. `COMPARE`, `MEMCONVERT`) stays diagnosed |
| MX6 | `VALUE` named constants | 1w | `DECLARE name ... VALUE (const)`; read-only storage initialized once, reassignment diagnosed at every write position; non-scalar/FILE/DEFINED/BASED and INITIAL combinations diagnosed, not guessed (ADR-108) |
| MX7 | `PACKAGE` / `EXPORTS` structure | 1w | package blocks as link-level namespaces generalizing ADR-103 external linkage; unexported names stay module-private; `FETCH`/`RELEASE` explicitly excluded (platform loading, stretch only) |

## Phase MX-C — preprocessor completion (2–3 engineer-weeks)

| Order | Slice | Effort | Deliverable |
|---|---|---:|---|
| MX8 | Iterative/conditional preprocessing | 2–3w | `%ACTIVATE`/`%DEACTIVATE` gating bare-`%NAME` substitution done, so ordinary parameterized includes work (ADR-113); `%DO` loops still diagnosed; full macro evaluation stays diagnosed |

## Stretch (unestimated, admit one at a time)

`ORDINAL`, `DEFINE ALIAS`, extended `DATETIME` patterns, `FETCH`/`RELEASE`.
Each needs its own Enterprise-semantics verification note before planning;
none is approved by this document.

## Exit criterion

A modern-idiom program using `SELECT` with loop exits, `%INCLUDE` with
parameters, `VALUE` constants, and the MX5 built-ins compiles without
source rewriting and prints checked reference output; the Extensions
ledger lists every admitted nicety with its ADR; `SELECT`/`LEAVE` as
plain identifiers still compile (no-reserved-words guard test).
