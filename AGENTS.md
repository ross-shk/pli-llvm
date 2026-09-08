# plic — Agent Guide

PL/I → LLVM compiler, built to `TR25.084-concrete-syntax.md` (syntax) and
Y33-6003 (semantics). Full workflow: `CONTRIBUTING.md`.

## Rules
- do not duplicate info from the specs into REAMDE and the docs unless actually necessary
- keep docs clear and concise, avoid lengthy explanations
- do not commit without my approval, make sure changes are atomic with clear commit messages
- do not quietly change/revert existing code unless required by the current task
- KISS: smallest change that works; keep implementations lean
- `tests/` is the source of truth — fix the compiler, not the test
- unimplemented ≠ accepted: diagnose it with its rule number (see Invariants)
- follow the indentation and style of the file you are editing
- one-line comments stating the *intent* of the block that follows
- PL/I card margins 2–72: nonblank `.pli`/`.inc` lines carry one leading space (text begins in column 2) and nothing past column 72

## Layout
| Path | Contents |
|---|---|
| `src/` | `lexer` → `parser` → `sema` → `irgen` (+ `diag`, `types`, `ast`, `main`) |
| `runtime/` | `libpli`: list-directed I/O, string semantics, conditions (C11) |
| `tests/` | `run_tests.sh` + groups: golden (`expected/*.out` diff) or self-checking (prints PASS), `bad_*` must fail |
| `docs/` | ARCHITECTURE, DESIGN-DECISIONS (ADRs), OPTIMIZATION, IMPLEMENTATION-PLAN, GRAMMAR-COVERAGE |
| `examples/` | scratch programs, git-ignored |
| `TR25.084-concrete-syntax.md` | the spec: rules (1)–(151), with ⚠ notes where the scan was damaged |

## Build
```bash
make && make test                        # 14 tests, must stay green
./build/plic f.pli -o f                  # compile
./build/plic f.pli -emit-llvm -o f.ll    # inspect IR (do this for codegen work)
./build/plic f.pli -fsyntax-only -v      # front end only / show clang command
```

## Invariants
1. lexer never classifies keywords — PL/I has no reserved words (`tests/core/keywords.pli`)
2. gaps are diagnosed with a rule number, never silently accepted
3. `d_.error(loc, msg, "(nn)")` — diagnostics cite TR 25.084
4. `IRGen` is the only LLVM-aware component (ADR-002)
5. ADRs are immutable — add a new number, never edit one

## Feature work
1. find the rule in `docs/GRAMMAR-COVERAGE.md` — not in TR 25.084 ⇒ out of scope, say so
2. test first (`tests/core/x.pli` golden or `tests/usecases/` self-checking, lowercase PL/I), then AST kind → parse → sema → irgen → runtime
3. `make test`; golden: record expected output, **read it**; self: print PASS
4. update `GRAMMAR-COVERAGE.md`; add an ADR if a decision was made
5. stage; report rules moved + test counts

## PL/I style
- new code: modern lowercase, `.pli` (`.inc` for includes), one leading space
- not sign: write `^` (or `~`); the UTF-8 glyph lexes too, but doubly-encoded bytes (mojibake) are diagnosed, never repaired
- `references/code/` is a *later-dialect* corpus (`select`, `do until`) — do not
  copy features from it into this compiler

## Token economy
- `grep` for a symbol first, then `read` with `offset`/`limit`
- large files: `src/parser.cpp`, `src/irgen.cpp`, `TR25.084-concrete-syntax.md`
- `edit` over `write`; batch related edits; parallel independent `bash` calls
- reproduce a bug (compile it, read the diagnostic or IR) before editing code
