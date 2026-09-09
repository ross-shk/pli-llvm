# Grammar coverage — TR 25.084 rules (1)–(151)

The ledger that ties the implementation to the specification. Status values:

- **M0**, **M1** — implemented in the completed milestones and covered by tests
- **diag** — recognised and rejected with a citation of its rule number
- **M2**-**M9** — planned for that milestone (IMPLEMENTATION-PLAN.md)
- **D1** — explicitly deferred specialized scalar conformance; diagnosed until implemented

| Rules | Feature | Status | Component / test |
|---|---|---|---|
| (1) | `program ::= procedure•••` | M0 | `parser.cpp:parse` / all tests |
| (2) | procedure, entry-namelist, options | M0 | `parseExternalProcedure` / `hello.pli` |
| (3) | entry-namelist (multiple entry names) | M0 | `a, b: PROCEDURE` shares one body; callable by any name (`multientry.pli`); extra names are internal aliases |
| (4) | parameterlist | M0 | `procs.pli` |
| (5) | procedure options (`OPTIONS`, `RECURSIVE`, `RETURNS`) | M0/M1 | `MAIN` honoured; `RETURNS` → function procedures (`func.pli`); `RECURSIVE` accepted |
| (6),(7) | sentencelist, end-clause, multiple closure | M0 | `parseBody` / `loops.pli` (`END OUTER;`) |
| (8) | sentence kinds | M0 | `parseStatement`; internal procedures reach enclosing automatic storage via a static link (ADR-027, `staticlink.pli`) |
| (9),(10) | `DECLARE`, declarationlist | M0 | `parseDeclare` / `ifelse.pli` |
| (11) | declaration, level numbers, factoring | M2 | scalars + level-numbered structures (nested via `1 S, 2 A ..., 2 X, 3 Y ...`) in read/write positions, including array members `2 A(10) ...` (`struct.pli`, `struct_array.pli`); factored lists `DECLARE (A, B) FIXED BIN(31)` with a factored dimension `(X, Y) (5) ...` (`factor.pli`); `INITIAL` in factored form → diag |
| (12),(13) | dimension attribute, bound pairs | partial M2 | fixed-size constant-bounds arrays, single- and multi-axis, row-major layout (`A(m,n)`/`A(lb:ub,...)`), scalar elements (`array.pli`, `array2d.pli`); dynamic bounds, `*` extents, cross-sections → M2 |
| (14),(15) | attribute, data-attribute set | partial M0 | arithmetic/string/`ALIGNED` subset |
| (16),(17) | arithmetic attributes, precision, signed integer | partial M0 | practical binary forms first; full decimal/precision conformance → D1 |
| (18) | string attributes (`BIT`/`CHARACTER`/`VARYING`) | partial M0 | `BIT(1)` and char/varying served (`strings.pli`); `BIT(n>1)` diagnosed as unimplemented (`bad_bitlen.pli`, rule (18)); schedule by corpus impact |
| (19) | `PICTURE` attribute | D1 | ADR-017 |
| (20) | `AREA` attribute | M3 | |
| (21) | `LABEL` attribute | M4 | label variables |
| (22) | `OFFSET` attribute | M3 | |
| (23) | storage classes | diag → M3 | `AUTOMATIC`/`STATIC` accepted, `CONTROLLED` M3 |
| (24) | `DEFINED`/`POSITION` | M2 | ADR-018; after ordinary aggregates |
| (25) | `BASED` | M3 | |
| (26)–(32) | `INITIAL` (incl. `CALL`, iteration, `*`) | partial M0 | scalar constants (M0); arrays: an `INITIAL` itemlist with iteration factors `(n)`, `*` repeat-last, and nested groups fills a fixed-size numeric/BIT array element-by-element on AUTOMATIC storage (`init_array.pli`, `bad_init_array.pli`, ADR-044); a count that does not match the extent is diagnosed; `INITIAL CALL` diagnosed (rule (27)); `INITIAL` on a structure diagnosed |
| (33) | non-data attributes | M8 | prioritize by corpus impact |
| (34)–(38) | `ENTRY`, `RETURNS`, descriptors, `USES`/`SETS` | partial M0 → M2 | ADR-021: scalar C entry/calls served; fixed-size arrays and structures passed by reference to PL/I procedures (`array_param.pli`, `struct.pli`); dope-vector descriptors for dynamic extents/`*` → M2 |
| (39),(40) | `FILE` attributes | M5 | |
| (41) | `GENERIC` | M8 | generic selection; prioritize by corpus impact |
| (42) | scope (`INTERNAL`/`EXTERNAL`) | M1 | linkage implemented |
| (43) | `LIKE` | M2 | `LIKE <structure-variable>` copies the template's structure shape onto the declared item — top-level `1 T LIKE S;` (T is a copy of S) and nested `2 M LIKE S;` (M is a member of S's shape) both work (`like.pli`, ADR-045); a non-structure or undeclared template is diagnosed, and LIKE combined with a dimension or with members is diagnosed (`bad_like.pli`); qualified LIKE templates `LIKE S.A.B` diagnosed |
| (44)–(55) | `FORMAT` statement and format items | M5 (picture items D1) | common format engine first |
| (56) | `ENTRY` statement | M1 | `label: ENTRY(params) [RETURNS(...)]` declares an alternate entry point with its own params (any count) and result type; body split into segments behind a shared impl, one thunk per entry name (`entry.pli`); mixed return types diagnosed unimplemented (ADR-026) |
| (57)–(59) | statement, unconditional, simple | M0 | |
| (60)–(63) | condition prefixes | parsed M0 → M4 | warned as unenforced |
| (64) | labellist | M0 | label prefixes parsed; used by (7) |
| (65) | initial-label (subscripted labels) | M2/M4 | scan damaged; see ⚠ in grammar |
| (66),(67) | proper-statement, null statement | M0 | |
| (68) | `BEGIN` block | M1 | executes as a real lexical scope; inner declarations shadow outer ones and do not leak (`begin.pli`); block variables are AUTOMATIC in the enclosing procedure's frame (ADR-010, ADR-027) |
| (69)–(73) | `DO` groups, specifications, `WHILE` | M0 | `loops.pli` |
| (74)–(76) | `IF`/`THEN`/`ELSE`, balanced statements | M0 | `ifelse.pli` |
| (77) | `GO TO` | M1/M4 | local `GO TO`/`GOTO` implemented (`goto.pli`, `bad_goto.pli`); non-local to an enclosing procedure → M4 |
| (78)–(80) | `CALL`, options, argumentlist | M0 (opts M9) | `procs.pli`; `TASK`/`EVENT` → M9 |
| (81) | `RETURN` | M0/M1 | plain `RETURN` (M0); `RETURN(value)` for function procedures (M1, `func.pli`) |
| (82),(83) | `WAIT`, `DELAY` | M9 | |
| (84),(85) | `EXIT`, `STOP` | M0 | |
| (86) | assignment (incl. `BY NAME`) | partial M2 | scalar single target; `SUBSTR` pseudo-variable (`substr_assign.pli`, ADR-024); whole-structure assignment between identical-shape structures (`struct_assign.pli`, rule (127)); multiple assignment `a, b, c = e` with one shared RHS and same-type scalar/array-element targets (`multiassign.pli`, `bad_multiassign.pli`, `bad_multiassign_type.pli`); `BY NAME` assignment `S = T, BY NAME` copies same-named members regardless of layout, recursing into minor structures and arrays, skipping names absent from either side (`struct_by_name.pli`, ADR-043); non-structure or multi-target `BY NAME` diagnosed (`bad_struct_byname.pli`) |
| (87)–(90) | `ALLOCATE`/`FREE` | diag → M3 | |
| (91)–(99) | conditions, `ON`/`REVERT`/`SIGNAL`, `CHECK` | diag → M4 | |
| (100)–(103) | `OPEN`/`CLOSE` | diag → M5 | |
| (104),(105) | `GET`/`PUT` and options | partial M0 | `PUT [SKIP] [PAGE] LIST`; rest → M5 |
| (106)–(111) | data specifications, data lists | partial M0 | list-directed output; common `DATA`/`EDIT` → M5, picture-directed forms → D1 |
| (112),(113) | record I/O | diag → M6 | option set from Y33-6003 (scan incomplete) |
| (114) | `DISPLAY` | M5 | scan garbled; Y33-6003 form used |
| (115)–(122) | expression precedence hierarchy | M0 | `arith.pli` pins `-3**2` = `-(3**2)` = -9 ((128) constants are unsigned); `usecases/expr.pli` pins negated comparisons and a prefixed `**` exponent |
| (118) | comparison operators | M0 | incl. `¬=`, `¬>`, `¬<` |
| (123) | primitive expressions | partial M2 | constants/vars (M0); function references to function procedures (`func.pli`); array attribute built-ins `LBOUND`/`HBOUND`/`DIM` on fixed-size single-axis arrays (`array_bounds.pli`, `bad_array_builtin.pli`); `DIM` of a multi-axis array = total element count; array reduction built-ins `SUM`/`PROD`/`ANY`/`ALL` over the full extent (`array_reduce.pli`, `bad_array_reduce.pli`); attribute/reduction built-ins also apply to parameter arrays inside a callee (`array_param.pli`); cross-sections/multi-axis → M2 |
| (124),(125) | locator qualification, qualified names | partial M2 | aggregate (structure) member qualification `S.A.B` in read/write positions and expressions (`struct.pli`); whole-structure value/assignment and `INITIAL` diagnosed (`bad_struct_*.pli`, rule (127)); locators → M3 |
| (126) | subscripted references | partial M2 | fixed-size constant-bounds scalar arrays in read/write positions, single- and multi-axis, compile-time + runtime SUBSCRIPTRANGE on every axis (`array.pli`, `array2d.pli`, `bad_array_oob.pli`, `bad_array2d_oob.pli`, `bad_array2d_arity.pli`); subscripted structure array members `S.A(i)` incl. nested/multi-axis (`struct_array.pli`, `bad_struct_array_*.pli`); parameter arrays passed by reference, subscriptable on both sides of an assignment inside a callee (`array_param.pli`); single-`*` cross-sections `A(i, *)`/`A(*, j)` (and of an array member `S.A(i, *)`) assign the reduced 1-D row/column value to a same-shape whole array (`cross_section.pli`, ADR-046); a cross-section to a scalar or non-matching target, more than one `*`, or a cross-section used as a general value is diagnosed (`bad_cross_section.pli`, rule (126)) |
| (127) | unsubscripted reference | partial M2 | whole-structure assignment served as a storage copy between structures of identical shape, incl. array members and nested members (`struct_assign.pli`); shape mismatch or mixing a structure with a non-structure diagnosed (`bad_struct_assign.pli`, `bad_struct_value.pli`, rule (127)); a whole structure as an expression value, and full aggregate references → M2 |
| (128),(129) | constants, replicated string constants | partial M0 | replicated strings served; imaginary/sterling → D1 |
| (130)–(133) | identifier, letter, alphameric, digit | M0 | incl. `$ # @` and break character |
| (134) | `isub` (`integer SUB`) | M2 | ADR-018; after ordinary aggregates |
| (135)–(139) | integer, fixed/float/imaginary constants | partial M0 | practical binary constants served; decimal/complex/imaginary completeness → D1 |
| (140)–(144) | string constants, bit strings, characters | M0 | `strings.pli` (incl. `''` escape) |
| (145) | sterling constants | D1 | with sterling pictures |
| (146)–(148) | picture specification/string/characters | D1 | ADR-017 |
| (149)–(151) | space, comment, comment symbols | M0 | `/* … */` |

## Auxiliary sections

| Section | Feature | Status |
|---|---|---|
| §2.1, §2.2 | notation semantics / meta-syntax | reference only |
| §2.3.1 | generation process, delimiters, 60-char alphabet | M0 (lexer) |
| §2.3.1 | statement-keyword disambiguation | M1 | ADR-004 step 3: `WORD ( … ) =` resolves by a speculative parse of the keyword reading (`ambiguity.pli`); `WORD =` stays an assignment |
| §2.3.2.1 | keyword abbreviations (`DCL`, `PROC`, `BIN`, …) | partial M0 (`DCL`, `PROC`, `BIN`, `DEC`, `CHAR`, `VAR`, `INIT`, `PTR`, `CTL`, `DEF`) → M8 completes the table |
| §2.3.2.2 | multiple closure | M0 |
| §2.3.3 | 48-character set: operator words, deletions, colon rules | partial M0 (operator words in `arith.pli`) → M8 |

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
| (26) | `emitInitials` | `INITIAL` stores on AUTOMATIC vars; an array itemlist expands via `sym->initElems` to per-element stores (`initValue`), and `emitGlobals` builds a constant-array initializer (`scalarInitConstant`) for STATIC |
| (34) | `calleeFn` | external C `ENTRY` decl (rule 38 interop) |
| (56) | `emitMultiEntryProc`, `entryIrName` | alternate entry thunks |
| (68) | `emitStmt` (`Begin`) | block body executed as a group |
| (69)–(73) | `emitDoWhile`, `emitDoIter` | `DO WHILE` / iterative |
| (74)–(76) | `emitIf` | |
| (77) | `collectGotoBlocks`, `emitStmt` (`Goto`) | local `GO TO` |
| (78),(79) | `emitCall` | `CALL` statement |
| (81) | `emitStmt` (`Return`) | value / plain `RETURN` |
| (84),(85) | `emitStmt` (`Stop`) | `pli_stop` + `Unreachable` |
| (86) | `emitAssign`, `storeTo` | incl. `SUBSTR` pseudo-variable (`pli_substr_assign`); multiple assignment evaluates the RHS once and stores to each target (`emitAssign` multi-target path); `BY NAME` emits member-by-name copies through `emitByNameCopy` (scalar load/convert/store, memcpy for arrays, recursion into minor structures) |
| (104),(105) | `emitPut` | list-directed output |
| (115)–(122) | `emitExpr` (binary/`Unary`) | arithmetic, bit, comparison, concat |
| (123) | `emitExpr` (`Call`) | per-builtin handlers: SUBSTR, INDEX, ABS, LENGTH, TRUNC, PRECISION, MIN, MAX, MOD, MULTIPLY, DIVIDE, ROUND, REPEAT, VERIFY, TRANSLATE, HIGH, LOW, DATE, TIME, LBOUND, HBOUND, DIM, SUM, PROD, ANY, ALL |
| (12),(13),(126) | `arrayExtent`, `arrayElementAddr`, `loadArrayElement`, `storeArrayElement` | `[N x elemTy]` flat row-major layout; multi-axis GEP offset = Σ (i_k − lb_k)·stride_k; per-axis SUBSCRIPTRANGE; `DIM` = product of extents; a structure array member lays out as `[N x elemTy]` inside the struct (`llvmTy` handles `isArray`) and is subscripted via `memberAddr` + `arrayElementAddr` |

## Headline numbers (M0)

- Rules fully implemented and tested: **40**
- Rules partially implemented: **18**
- Rules recognised and diagnosed with their number: **21**
- Rules not yet reached: **72**
