# Grammar coverage — TR 25.084 rules (1)–(151)

The ledger that ties the implementation to the specification. Status values:

- **M0** — implemented in the wireframe and covered by a test
- **diag** — recognised and rejected with a citation of its rule number
- **Mn** — planned for that milestone (IMPLEMENTATION-PLAN.md)

| Rules | Feature | Status | Component / test |
|---|---|---|---|
| (1) | `program ::= procedure•••` | M0 | `parser.cpp:parse` / all tests |
| (2) | procedure, entry-namelist, options | M0 | `parseExternalProcedure` / `hello.pli` |
| (3) | entry-namelist (multiple entry names) | M0 | `a, b: PROCEDURE` shares one body; callable by any name (`multientry.pli`); extra names are internal aliases |
| (4) | parameterlist | M0 | `procs.pli` |
| (5) | procedure options (`OPTIONS`, `RECURSIVE`, `RETURNS`) | partial M0 | `MAIN` honoured; `RETURNS` → function procedures (`func.pli`); `RECURSIVE` accepted |
| (6),(7) | sentencelist, end-clause, multiple closure | M0 | `parseBody` / `loops.pli` (`END OUTER;`) |
| (8) | sentence kinds | M0 | `parseStatement`; internal procedures reach enclosing automatic storage via a static link (ADR-027, `staticlink.pli`) |
| (9),(10) | `DECLARE`, declarationlist | M0 | `parseDeclare` / `ifelse.pli` |
| (11) | declaration, level numbers, factoring | partial M0 | scalars; factoring and levels → M3 |
| (12),(13) | dimension attribute, bound pairs | M3 | arrays |
| (14),(15) | attribute, data-attribute set | partial M0 | arithmetic/string/`ALIGNED` subset |
| (16),(17) | arithmetic attributes, precision, signed integer | partial M0 | scale 0 only; full → M2 |
| (18) | string attributes (`BIT`/`CHARACTER`/`VARYING`) | partial M0 | `BIT(1)` and char/varying served (`strings.pli`); `BIT(n>1)` diagnosed as unimplemented → M2 (`bad_bitlen.pli`, rule (18)) |
| (19) | `PICTURE` attribute | M2 | ADR-017 |
| (20) | `AREA` attribute | M4 | |
| (21) | `LABEL` attribute | M5 | label variables |
| (22) | `OFFSET` attribute | M4 | |
| (23) | storage classes | diag → M4 | `AUTOMATIC`/`STATIC` accepted, `CONTROLLED` M4 |
| (24) | `DEFINED`/`POSITION` | M3 | ADR-018 |
| (25) | `BASED` | M4 | |
| (26)–(32) | `INITIAL` (incl. `CALL`, iteration, `*`) | partial M0 | scalar constants; full → M3 |
| (33) | non-data attributes | M2 | |
| (34)–(38) | `ENTRY`, `RETURNS`, descriptors, `USES`/`SETS` | partial M0 | ADR-021: `DECLARE … ENTRY` external C entry + by-ref call (`cinterop`); `RETURNS` function procedures (scalar result, `func.pli`); full descriptors/`USES`/`SETS` pending |
| (39),(40) | `FILE` attributes | M6 | |
| (41) | `GENERIC` | M2 | generic selection |
| (42) | scope (`INTERNAL`/`EXTERNAL`) | accepted M0 → M1 | linkage in M1 |
| (43) | `LIKE` | M3 | |
| (44)–(55) | `FORMAT` statement and all format items | M6 | format engine |
| (56) | `ENTRY` statement | M1 | `label: ENTRY(params) [RETURNS(...)]` declares an alternate entry point with its own params (any count) and result type; body split into segments behind a shared impl, one thunk per entry name (`entry.pli`); mixed return types diagnosed unimplemented (ADR-026) |
| (57)–(59) | statement, unconditional, simple | M0 | |
| (60)–(63) | condition prefixes | parsed M0 → M5 | warned as unenforced |
| (64) | labellist | M0 | label prefixes parsed; used by (7) |
| (65) | initial-label (subscripted labels) | M3/M5 | scan damaged; see ⚠ in grammar |
| (66),(67) | proper-statement, null statement | M0 | |
| (68) | `BEGIN` block | partial M1 | executes; a block is now a real lexical scope — inner declarations shadow outer ones and do not leak (`begin.pli`); block variables are AUTOMATIC in the enclosing procedure's frame (ADR-010, ADR-027) |
| (69)–(73) | `DO` groups, specifications, `WHILE` | M0 | `loops.pli` |
| (74)–(76) | `IF`/`THEN`/`ELSE`, balanced statements | M0 | `ifelse.pli` |
| (77) | `GO TO` | partial M1 | local `GO TO`/`GOTO` to a label in the same procedure (`goto.pli`, `bad_goto.pli`); non-local to an enclosing procedure → M5 |
| (78)–(80) | `CALL`, options, argumentlist | M0 (opts M9) | `procs.pli`; `TASK`/`EVENT` → M9 |
| (81) | `RETURN` | partial M0 | plain `RETURN` (M0); `RETURN(value)` for function procedures (M1, `func.pli`) |
| (82),(83) | `WAIT`, `DELAY` | M9 | |
| (84),(85) | `EXIT`, `STOP` | M0 | |
| (86) | assignment (incl. `BY NAME`) | partial M0 | scalar single target; `SUBSTR` pseudo-variable (`substr_assign.pli`, ADR-024); multiple/`BY NAME` → M3 |
| (87)–(90) | `ALLOCATE`/`FREE` | diag → M4 | |
| (91)–(99) | conditions, `ON`/`REVERT`/`SIGNAL`, `CHECK` | diag → M5 | |
| (100)–(103) | `OPEN`/`CLOSE` | diag → M6 | |
| (104),(105) | `GET`/`PUT` and options | partial M0 | `PUT [SKIP] [PAGE] LIST`; rest → M6 |
| (106)–(111) | data specifications, data lists | partial M0 | list-directed output; `DATA`/`EDIT` → M6 |
| (112),(113) | record I/O | diag → M7 | option set from Y33-6003 (scan incomplete) |
| (114) | `DISPLAY` | M6 | scan garbled; Y33-6003 form used |
| (115)–(122) | expression precedence hierarchy | M0 | `arith.pli` pins `-3**2` = `-(3**2)` = -9 ((128) constants are unsigned); `usecases/expr.pli` pins negated comparisons and a prefixed `**` exponent |
| (118) | comparison operators | M0 | incl. `¬=`, `¬>`, `¬<` |
| (123) | primitive expressions | partial M0 | constants/vars (M0); function references to function procedures (`func.pli`) |
| (124),(125) | locator qualification, qualified names | diag → M3/M4 | |
| (126) | subscripted references | diag → M3 | incl. `*` cross-sections |
| (127) | unsubscripted reference | M3 | |
| (128),(129) | constants, replicated string constants | partial M0 | replicated string constants `(n)'str'` expanded at parse time (rule (129), `repl.pli`); imaginary/sterling → M2 |
| (130)–(133) | identifier, letter, alphameric, digit | M0 | incl. `$ # @` and break character |
| (134) | `isub` (`integer SUB`) | M3 | ADR-018 |
| (135)–(139) | integer, fixed/float/imaginary constants | partial M0 | `B` radix parsed; `COMPLEX` → M2 |
| (140)–(144) | string constants, bit strings, characters | M0 | `strings.pli` (incl. `''` escape) |
| (145) | sterling constants | M2 | with sterling pictures |
| (146)–(148) | picture specification/string/characters | M2 | ADR-017 |
| (149)–(151) | space, comment, comment symbols | M0 | `/* … */` |

## Auxiliary sections

| Section | Feature | Status |
|---|---|---|
| §2.1, §2.2 | notation semantics / meta-syntax | reference only |
| §2.3.1 | generation process, delimiters, 60-char alphabet | M0 (lexer) |
| §2.3.1 | statement-keyword disambiguation | M1 | ADR-004 step 3: `WORD ( … ) =` resolves by a speculative parse of the keyword reading (`ambiguity.pli`); `WORD =` stays an assignment |
| §2.3.2.1 | keyword abbreviations (`DCL`, `PROC`, `BIN`, …) | partial M0 (`DCL`, `PROC`, `BIN`, `DEC`, `CHAR`, `VAR`, `INIT`, `PTR`, `CTL`, `DEF`) → M9 completes the table |
| §2.3.2.2 | multiple closure | M0 |
| §2.3.3 | 48-character set: operator words, deletions, colon rules | partial M0 (operator words in `arith.pli`) → M9 |

## IRGen touch-points

Where each implemented feature lands in codegen (`src/irgen.cpp`). The rule →
emit mapping is the same "one feature = N edits" chain as CONTRIBUTING.md; the
entry points below are where that chain terminates.

| Rule(s) | IRGen entry point | Notes |
|---|---|---|
| (1),(2),(3) | `declareProc`, `emitPlainProc`, `emitMultiEntryProc` | procedure shapes and entry aliases |
| (4) | `emitCall` | dummy-argument materialisation for by-ref params |
| (5) | `declareProc` | `MAIN` shim + `RETURNS` function procs |
| (6),(7),(8) | `emitStmt` → `emitAssign`/`emitIf`/… | statement dispatch |
| (9),(10) | `emitGlobals` | `DECLARE` storage |
| (16) | `emitExpr` (`Call`) | `TRUNC` scaled-FIXED guard diagnosed in sema |
| (18) | `loadSym`/`storeTo` | `BIT(1)` held as `i8` in registers (`toI1`) |
| (26) | `emitInitials` | `INITIAL` stores on AUTOMATIC vars |
| (34) | `calleeFn` | external C `ENTRY` decl (rule 38 interop) |
| (56) | `emitMultiEntryProc`, `entryIrName` | alternate entry thunks |
| (68) | `emitStmt` (`Begin`) | block body executed as a group |
| (69)–(73) | `emitDoWhile`, `emitDoIter` | `DO WHILE` / iterative |
| (74)–(76) | `emitIf` | |
| (77) | `collectGotoBlocks`, `emitStmt` (`Goto`) | local `GO TO` |
| (78),(79) | `emitCall` | `CALL` statement |
| (81) | `emitStmt` (`Return`) | value / plain `RETURN` |
| (84),(85) | `emitStmt` (`Stop`) | `pli_stop` + `Unreachable` |
| (86) | `emitAssign`, `storeTo` | incl. `SUBSTR` pseudo-variable (`pli_substr_assign`) |
| (104),(105) | `emitPut` | list-directed output |
| (115)–(122) | `emitExpr` (binary/`Unary`) | arithmetic, bit, comparison, concat |
| (123) | `emitExpr` (`Call`) | per-builtin handlers: SUBSTR, INDEX, ABS, LENGTH, TRUNC, PRECISION, MIN, MAX, MOD, MULTIPLY, DIVIDE, ROUND, REPEAT, VERIFY, TRANSLATE, HIGH, LOW, DATE, TIME |

## Headline numbers (M0)

- Rules fully implemented and tested: **40**
- Rules partially implemented: **18**
- Rules recognised and diagnosed with their number: **21**
- Rules not yet reached: **72**
