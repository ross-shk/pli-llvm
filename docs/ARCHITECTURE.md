# plic — Architecture

A modern PL/I compiler with an LLVM backend, specified against
**TR 25.084, *Concrete Syntax of PL/I*** (IBM Laboratory Vienna, 1968) with
semantics from the **Y33-6003** language specifications. Both are in
`references/`; the extracted grammar is `TR25.084-concrete-syntax.md`.

This page describes the implementation that exists today. The target language
is broader than current support; use `GRAMMAR-COVERAGE.md` for implemented and
diagnosed forms. In particular, the planned machine-independent IR (MIR) in
`OPTIMIZATION.md` does not exist yet: the current HIR lowers directly to LLVM
IR.

## 1. Goals and non-goals

**Design goal: generate efficient native code without changing PL/I behavior.**
The current backend emits LLVM IR and delegates optimization and machine-code
generation to clang. The more detailed HIR/MIR optimization strategy in
`OPTIMIZATION.md` is planned work, not a description of passes already present.

**Goals**

1. Accept the language defined by TR 25.084 rules (1)–(151), i.e. full
   PL/I as of the 1968 specifications: aggregates, `BASED`/`CONTROLLED`
   storage, areas and offsets, conditions and `ON`-units, stream and record
   I/O, `PICTURE` data, decimal arithmetic, multitasking.
2. Native code quality competitive with C for the scalar/array subset, via
   LLVM's optimizer.
3. Diagnostics of modern quality: source spans, spec rule citations, fix-it
   hints, no cascading errors.
4. Interoperate with C ABIs so PL/I can be introduced into existing systems
   incrementally (`OPTIONS(BYVALUE)`-style extensions, C-compatible structs).
5. Be a *usable migration target* for legacy IBM PL/I: compatibility modes for
   the 48-character set, EBCDIC source, `%INCLUDE`, and IBM extensions.

**Non-goals**

1. Bit-exact reproduction of IBM S/360 floating point or storage layout
   (offered per-declaration, not globally).
2. Source-level compatibility with dialect-specific IBM preprocessor macros
   beyond `%INCLUDE`/`%IF`/`%DECLARE`.
3. Preserving PL/I's undefined behaviour where the spec leaves it undefined —
   we define it and document it.

## 2. Pipeline

```
file.pli
   │
   ▼
Preprocessor       expands supported directives and `%INCLUDE`
   │
   ▼
Lexer              creates tokens; words are not classified as keywords
   │
   ▼
Parser             builds the AST and recognizes keywords by context
   │
   ▼
Semantic analysis  resolves names, types, attributes, and conversions
   │
   ▼
HIR                typed representation with explicit conversions
   │
   ▼
IRGen              builds an LLVM module with `llvm::IRBuilder`
   │
   ├── `-emit-llvm` ──► textual `.ll` file
   │
   └── clang ──► object file ──► link with `libpli` ──► executable
```

### Why separate AST, HIR, and LLVM IR

The AST records source syntax. HIR is a typed, lower-level representation that
makes implicit conversions explicit before code generation. This separation
keeps parsing and semantic analysis independent of LLVM.

The original design planned a fourth, machine-independent IR (MIR) between HIR
and LLVM. It would lower PL/I-specific aggregates, checks, and control flow
before LLVM code generation. That layer is still future work; do not assume
that passes described for MIR are present in the current compiler.

## 3. Components

| Component | Files | Responsibility |
|---|---|---|
| Driver | `src/main.cpp` | option parsing, phase sequencing, sub-process invocation |
| Diagnostics | `src/diag.{h,cpp}` | locations, severity, rule citations, caret output |
| Preprocessor | `src/preprocessor.{h,cpp}` | `%INCLUDE` and supported preprocessor directives; diagnostics for unsupported forms |
| Lexer | `src/lexer.{h,cpp}`, `src/token.h` | rules (130)–(151); comments; composite operators; not-symbol spellings |
| Parser | `src/parser.{h,cpp}` | rules (1)–(129); keyword recognition; multiple closure; recovery |
| AST | `src/ast.h` | syntax tree |
| Types | `src/types.h` | attribute → type mapping |
| Sema | `src/sema.{h,cpp}` | scopes, declarations, defaults, typing, conversions |
| HIR | `src/hir.{h,cpp}` | typed AST lowering with explicit conversions (ADR-005); `--print-hir` |
| IR generation | `src/irgen.{h,cpp}` | LLVM IR via `llvm::IRBuilder<>` (ADR-002); consumes HIR |
| Runtime | `runtime/` | C support routines for I/O, strings, conditions, storage, and other implemented features |

### 3.1 Source input

The driver passes the source file to the preprocessor; there is no separate
source-manager phase or CLI option for source encoding or card margins. The
lexer handles the source spellings supported by the implementation, including
the three not-symbol spellings.
### 3.2 Preprocessor

Expands `%INCLUDE member;` recursively before lexing. Members resolve relative
to the containing source file; a name without an extension falls back to
`.inc`. Quoted paths are also accepted. Include paths can be supplied with
`-I` or `PLIC_INCLUDE_PATH`. Directives inside comments and strings are ignored;
include cycles and missing members are diagnosed. The preprocessor also
supports the directives listed in the grammar-coverage ledger; other Chapter 9
forms are diagnosed rather than silently passed through.

### 3.3 Lexer

Produces `Word`, `Number`, `CharLit`, `BitLit`, punctuation and operators.
Two PL/I-specific obligations:

- **No keyword classification.** Keywords are recognised by the parser
  (ADR-004). The token stream for `IF IF = THEN THEN THEN = ELSE;` is nine
  words and two operators.
- **Composite operators are single words** (`**`, `||`, `>=`, `<=`, `¬=`,
  `¬>`, `¬<`, `->`), because TR §2.3.1 step 1 treats them as indivisible
  notation constants; the not-symbol is accepted as `¬`, `^` or `~`.

### 3.4 Parser

Recursive descent, one function per production group, plus precedence climbing
for expressions with the exact precedence of rules (115)–(122):

```
|  <  &  <  comparison  <  ||  <  + -  <  * /  <  ** / prefix + - ¬
```

Two features drive the design:

- **Positional keyword recognition** with bounded lookahead and speculative
  parsing plus symbol-table consultation — ADR-004.
- **Multiple closure** (TR §2.3.2.2): `END L;` closes every open block up to
  the one labelled `L`. Implemented by returning an `EndInfo` outward through
  the block-parsing functions until the owning block claims it.

Error recovery is statement-granular: a malformed statement is reported once
and the parser resynchronises on the next `;`, which suits a language whose
statements are unambiguously `;`-terminated.

### 3.5 Semantic analysis

Semantic analysis is organized into ordered passes. Declarations are collected
before uses are checked because attribute defaults and procedure signatures can
depend on declarations that appear later in the block:

1. **Block structure**: build the scope tree (external procedure, internal
   procedures, `BEGIN` blocks, `DO` groups do *not* introduce a scope).
2. **Declaration collection**: `DECLARE` (rule 9) including factored lists and
   level-numbered structures; `ENTRY`/`FILE`/label declarations; `LIKE`
   expansion; `DEFINED`/`iSUB` base resolution.
3. **Contextual declarations**: resolve names whose attributes come from
   context, such as `BASED` locators and `SET` targets.
4. **Implicit declarations**: undeclared identifiers get `FIXED BINARY` for
   initials I–N, otherwise `FLOAT DECIMAL`; implicit declarations produce a
   warning.
5. **Attribute defaults and conflict checking**: rules (14)–(43).
6. **Structure layout**: offsets and alignment for supported member types.
7. **Expression typing and conversion insertion**: the target-type algorithm
   for arithmetic (base/scale/mode/precision), string, and bit operands.
8. **Condition prefixes**: preserve supported enable/disable prefixes so
   code generation can emit or omit the corresponding checks.

This is a conceptual summary, not a promise that every rule is implemented.
The coverage ledger identifies unsupported declaration forms and conditions.

### 3.6 Runtime interface (libpli)

The compiler's LLVM module calls C runtime routines for operations that are not
emitted inline. These routines form the internal `pli_*` ABI. The specific
entry points and supported data types evolve with the feature set; consult the
runtime declarations and `GRAMMAR-COVERAGE.md` rather than treating this page
as an exhaustive ABI reference.

Common runtime responsibilities include list-directed and edit-directed I/O,
character operations, condition dispatch, dynamic storage, and task/event
support. Some language forms that would use these services are still diagnosed.

## 4. Data representation and ABI

| PL/I data | Representation | Notes |
|---|---|---|
| `FIXED BINARY` | `i32` or `i64`, based on precision | Scaled forms use an integer representation; see the coverage ledger for supported precision and scale cases. |
| `FIXED DECIMAL` | Scaled integer for supported precision | Scale is a compile-time property (ADR-006); this is not full decimal conformance. |
| `FLOAT` | `float`, `double`, or LLVM extended precision | Exact precision mappings depend on the declared type. |
| `CHARACTER(n)` | Byte buffer, blank padded | |
| `CHARACTER(n) VARYING` | Length plus byte buffer | Stores the current length as well as the maximum capacity. |
| Adjustable `CHARACTER` parameter | Caller buffer plus hidden length argument | Passed by reference. |
| `BIT(n)` | Packed bytes; `BIT(1)` uses a scalar representation in expressions | Supported operations vary by context. |
| `COMPLEX` | Pair of floating-point values | The pair stores the real and imaginary components. |
| `POINTER` | LLVM pointer | |
| structure | LLVM struct | Layout follows the supported structure mapping rules. |
| array | Contiguous, row-major storage | Dynamic bounds and parameter extents use runtime values. |

This table describes representation choices for supported forms, not a claim
that every listed PL/I type or attribute is complete. `PICTURE`, `AREA`,
`OFFSET`, label variables, and full entry descriptors remain limited or
unimplemented; see the coverage ledger.

**Parameter passing.** PL/I arguments are generally passed by reference: the
callee receives addresses. If an argument needs conversion, is an expression,
or is a constant, the caller materializes a **dummy argument** and passes its
address (`tests/core/procs.pli`). Adjustable arrays and character parameters
use additional hidden extent or length arguments. Internal procedures use a
static link to access enclosing automatic storage. Storage classes and
aggregate descriptors have restrictions documented in the coverage ledger.

**Linkage.** External procedures use their upper-cased PL/I name for linkage.
Package exports and other naming restrictions are described in the coverage
ledger.

**Entry point.** An `OPTIONS(MAIN)` procedure is exposed through a C `main`
entry that initializes and shuts down the runtime around the PL/I procedure.

## 5. Condition handling (`ON` units)

PL/I conditions have dynamic scope and can resume after a handler runs. The
compiler implements a subset of the condition rules; non-local `GO TO` and
many condition kinds are still diagnosed.

- The implemented `ON`/`SIGNAL`/`REVERT` paths include `ERROR`, `SIZE`,
  `SUBSCRIPTRANGE`, `ZERODIVIDE`, and programmer-named conditions. Check the
  coverage ledger for each condition's behavior and unsupported cases.
- Condition prefixes are resolved during compilation. Supported prefixes such
  as `(NOSIZE)` omit the matching runtime check; they are not runtime switches.
- On-unit control flow and recovery are implemented only for the supported
  condition subset. Do not assume general non-local `GO TO` or all standard
  condition actions are available.

## 6. Diagnostics

Every diagnostic carries a source location, and — where a syntactic rule is
implicated — the TR 25.084 production number, e.g.

```
tests/core/bad_attrs.pli:4:12: error: FIXED and FLOAT are conflicting attributes  [TR 25.084 rule (16)]
     DECLARE A FIXED FLOAT;
             ^
```

This is deliberate: PL/I's rule set is large and unfamiliar to most working
programmers, and a citation makes a diagnostic checkable against the spec.
`--explain <rule>` prints the TR 25.084 production for a rule number (generated
from the spec by `scripts/gen_rules.py`, so it cannot drift). A fix-it is an
insertion suggested at the caret column — e.g. `expected THEN` renders the
missing `THEN` under the offending token — for the unambiguous recovery cases
(missing `THEN`, `=`, `PROCEDURE`, or `END`). Planned: `-fdiagnostics-format=json`.

## 7. Testing architecture

| Layer | Mechanism | Status |
|---|---|---|
| IR checks | `tests/ir/*.pli` and ordered `*.check` patterns against `-emit-llvm` output | Implemented |
| Execution tests | Compile and run; compare `expected/` output or check self-reported `PASS` | Implemented |
| Diagnostic tests | `bad_*.pli` cases must fail as expected | Implemented |
| Coverage ledger | `GRAMMAR-COVERAGE.md` maps each rule to implementation notes and tests | Maintained with feature work |
| Corpus compilation, differential tests, grammar fuzzing | Proposed broader conformance tools | Roadmap; see implementation plans |

## 8. Current implementation snapshot

The compiler supports procedures, control flow, arrays and structures, storage
and pointer features, conditions, input/output, selected built-ins, and
preprocessor forms. Support is deliberately partial: a construct may be
accepted only for some types or contexts, while unsupported forms produce
diagnostics. Read the matching rule row in `GRAMMAR-COVERAGE.md` before relying
on a feature. That ledger is the current source for detailed behavior; the
milestone names in older planning documents describe when work was grouped,
not the current implementation boundary.
