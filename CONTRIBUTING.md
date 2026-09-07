# Contributing to plic

How to add a feature to the PL/I compiler. Written for humans and coding
agents: the workflow is the same, the invariants are non-negotiable.

Read first: `docs/ARCHITECTURE.md` (pipeline), `docs/GRAMMAR-COVERAGE.md`
(what exists), `docs/DESIGN-DECISIONS.md` (why). Feature scope comes from
`TR25.084-concrete-syntax.md` — the specification, not from other dialects.

## Build and test

```bash
make                       # build/plic + build/libpli.a
make test                  # compile, run, diff every tests/*/*.pli
./build/plic f.pli -o f    # compile a program
./build/plic f.pli -emit-llvm -o f.ll   # inspect generated IR
./build/plic f.pli -fsyntax-only        # front end only
./build/plic f.pli -v --keep-ll         # show clang command, keep the .ll
```

## Layer map — where a feature lands

| Feature kind | Touch | Example |
|---|---|---|
| New token / operator spelling | `src/lexer.cpp`, `src/token.h` | `¬` spellings |
| New statement | `parser.cpp:parseStatement` + a `parseXxx`, `ast.h:Stmt::Kind`, `sema.cpp:checkStmt`, `irgen.cpp:emitStmt` | `PUT` |
| New expression form | `parser.cpp:parseExpr/parsePrimary`, `sema.cpp:typeExpr`, `irgen.cpp:emitExpr` | `||` |
| New attribute | `parser.cpp:parseDeclItem`, `types.h`, `sema.cpp` | `VARYING` |
| New data type | `types.h` (+ `llvmTy`, `desc`), `irgen.cpp:convert/loadSym/storeTo` | `BIT(1)` |
| Runtime behaviour | `runtime/pli_rt.{h,c}` + `declare` in `irgen.cpp:run` | `pli_cmp_char` |
| Driver flag | `src/main.cpp` + README usage block | `-emit-llvm` |

One statement usually means five edits: AST kind → parse → check → emit →
test. Keep them in one change.

## Workflow

1. **Pick the rule.** Find the feature in `docs/GRAMMAR-COVERAGE.md` and note
   its rule number, e.g. `(104)-(109)` for stream I/O. If it is not in
   TR 25.084, it is out of scope — say so instead of implementing it.
2. **Write the test first.** `tests/core/<feature>.pli`, lowercase PL/I, with
   a header comment naming the rules exercised. Add `tests/core/bad_<feature>.pli`
   if the feature has error cases.
3. **Implement across the layers** in the table above, smallest change that
   works (KISS). Diagnose what you do not implement — never accept silently.
4. **Verify**: `make test`. Then record expected output:
   `./build/plic tests/core/x.pli -o /tmp/x && /tmp/x > tests/core/expected/x.out`
   Read that file before committing it — it is now the specification of
   behaviour, so a wrong line becomes a permanent wrong answer.
5. **Check the IR** for anything non-trivial: `-emit-llvm` and read it. Cheap,
   and catches silently-dropped work (see the worked example below).
6. **Update docs**: flip the row in `docs/GRAMMAR-COVERAGE.md`; adjust
   `docs/ARCHITECTURE.md` §8 status and the README feature list if user-visible;
   add an ADR to `docs/DESIGN-DECISIONS.md` if you made a real decision (new
   number, never edit an existing ADR); note deviations from the spec.
7. **Stage, do not commit.** `git add -A` and report what changed. Committing
   needs the maintainer's approval.

## Invariants

Break these and the design breaks:

1. **The lexer never classifies keywords.** PL/I has no reserved words; keyword
   recognition is positional, in `parser.cpp:atStmtKeyword` /
   `looksLikeAssignment`. `tests/core/keywords.pli` guards this.
2. **Unimplemented is diagnosed, never accepted.** Every gap produces an error
   citing its rule number: `d_.error(loc, "… is not implemented in this stage", "(91)")`.
   Silent acceptance produces wrong answers; an error is a to-do list entry.
3. **Diagnostics cite the specification.** Third argument to
   `d_.error`/`d_.warn` is the TR 25.084 rule, e.g. `"(16)"`.
4. **`tests/` is the source of truth.** Change the compiler to satisfy tests,
   not the reverse. Changing an `expected/*.out` file requires justification.
5. **`IRGen` is the only place that knows about LLVM.** Parser and sema must
   stay backend-agnostic; M1 swaps textual IR for the LLVM C++ API behind that
   interface (ADR-002).
6. **Layer separation**: lexing knows no grammar, parsing knows no types,
   sema knows no LLVM, codegen adds no diagnostics that sema could have made.
7. **Deviations get written down** — ADR or an explicit note in the docs, plus
   a diagnostic where the user could be surprised.

## Worked example — a real fix

`examples/if_else.pli` compiled cleanly and printed *nothing*:

```pli
 dcl x fixed bin(31) init(1);
 if x = 1 then put skip list('ok');
```

Diagnosis, in workflow order:

1. `-emit-llvm` showed `@pli_g_X = internal global i32 0` — the `INITIAL`
   value never reached the IR. Root cause: variables of the external procedure
   get static storage (ADR-010), and `irgen.cpp:emitGlobals` always emitted a
   zero initializer while the prologue-store path was reserved for automatic
   variables. The value was dropped between two correct-looking branches.
2. Test first: `tests/core/init.pli`, covering `INITIAL` for `FIXED`, `FLOAT`,
   `CHAR`, `CHAR VARYING`, `BIT`, a negative constant, and both storage
   classes.
3. Fix across layers: sema attaches the folded constant to the symbol
   (`sema.h:Symbol::initExpr`) and checks it is assignable to the declared
   type; `emitGlobals` renders it per type.
4. `make test` — 9/9, and `examples/if_else.pli` now prints `ok`.

The lesson worth generalising: **a feature that is "accepted" by the parser and
sema but ignored by codegen is worse than one that is rejected.** Invariant 2
exists because of this class of bug.

## Test conventions

| Convention | Meaning |
|---|---|
| `tests/<group>/x.pli` + `tests/<group>/expected/x.out` | compile, run, diff stdout |
| `tests/<group>/bad_x.pli` | must be rejected; `run_tests.sh` checks exit status |
| `tests/<group>/out/` | scratch binaries, logs and diffs; gitignored |
| Header comment | names the rules exercised, e.g. `rules (74),(75)` |
| Style | modern lowercase PL/I, one leading space, as in `tests/core/init.pli` |

`run_tests.sh` auto-discovers every `tests/*/` subfolder: a group is any
folder with `*.pli` programs and an `expected/` directory — create one and
it runs.

Existing uppercase tests stay as they are — follow the style of the file you
are editing.

## Code style

- C++20, `-Wall -Wextra` clean. Two-space indent, `lowerCamelCase` functions,
  `snake_case_` for private members with a trailing underscore as in
  `src/irgen.h`.
- One-line comment above a block stating its **intent**, not its mechanics.
  Cite rule numbers in comments where a production is implemented:
  `// if-statement ::= if-clause statement …  rules (74),(75)`.
- No exceptions, no RTTI, no dependencies beyond the standard library.
- Runtime is C11 and must stay free of C++ and of allocation on hot paths.
- Keep functions short enough to read whole; prefer adding a function to
  growing a `switch` arm past a screenful.

## Reviewing a change (checklist)

- [ ] rule number identified, and in TR 25.084 scope
- [ ] test added; `make test` green; expected output read and correct
- [ ] IR inspected for non-trivial codegen
- [ ] unimplemented sub-cases diagnosed with rule numbers
- [ ] `docs/GRAMMAR-COVERAGE.md` updated; ADR added if a decision was made
- [ ] no `expected/*.out` weakened; no invariant above violated
- [ ] staged, not committed

## Notes for coding agents

- Start with `docs/GRAMMAR-COVERAGE.md` to choose work and to know what is
  already there; do not infer features from other PL/I dialects found in
  `references/code/` — that corpus is later dialects (`select`, `do until`,
  `%process`) which this compiler deliberately rejects.
- Reproduce before fixing: compile the failing program, read the diagnostic or
  the IR, state the root cause in one sentence, then change code.
- Prefer `grep` for a symbol, then `read` with `offset`/`limit`; the large
  files are `src/parser.cpp`, `src/irgen.cpp`,
  `TR25.084-concrete-syntax.md`.
- Report: what changed, which rules moved, test counts before/after, and any
  deviation introduced. Leave the tree building and tests green, staged not
  committed.
