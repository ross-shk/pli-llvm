# plic — Architecture

A modern PL/I compiler with an LLVM backend, specified against
**TR 25.084, *Concrete Syntax of PL/I*** (IBM Laboratory Vienna, 1968) with
semantics from the **Y33-6003** language specifications. Both are in
`references/`; the extracted grammar is `TR25.084-concrete-syntax.md`.

## 1. Goals and non-goals

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
┌──────────────┐   %INCLUDE, margins, 48/60-char set, EBCDIC→UTF-8
│ SourceMgr    │   line map for diagnostics
└──────┬───────┘
       ▼
┌──────────────┐   optional; PL/I % preprocessor statements
│ Preprocessor │   (own lexer/parser reusing the same infrastructure)
└──────┬───────┘
       ▼
┌──────────────┐   words are never classified as keywords here (ADR-004)
│ Lexer        │   rules (130)-(151)
└──────┬───────┘
       ▼
┌──────────────┐   recursive descent + precedence climbing
│ Parser       │   positional keyword recognition, multiple closure
└──────┬───────┘   rules (1)-(129)
       ▼
    ┌─────┐
    │ AST │   faithful syntax; one node per production
    └──┬──┘
       ▼
┌──────────────┐   scopes; explicit/contextual/implicit declarations;
│ Sema         │   attribute defaults; conversions; aggregate typing;
└──────┬───────┘   condition enable-state; PICTURE compilation
       ▼
    ┌─────┐   PL/I-aware, still structured: aggregate assignment, string
    │ HIR │   ops, ON-units, DO semantics, descriptors implicit
    └──┬──┘
       ▼   ── HIR passes (see OPTIMIZATION.md §3)
    ┌─────┐   scalarised, explicit descriptors/temporaries, explicit
    │ MIR │   bounds checks, CFG with EH regions, no implicit conversions
    └──┬──┘
       ▼   ── MIR passes (see OPTIMIZATION.md §4)
┌──────────────┐
│ LLVM IR      │   llvm::Module via IRBuilder
└──────┬───────┘
       ▼   ── LLVM pass pipeline + our custom passes (OPTIMIZATION.md §5)
   object file ──► link with libpli ──► executable
```

### Why four levels

*AST → LLVM IR directly* (what M0 does) stops scaling as soon as the
PL/I-specific semantics arrive:

- An aggregate assignment `A = B + C;` over arrays/structures is a loop nest
  whose shape depends on matching declarations, `BY NAME`, and `UNALIGNED`
  packing. Expressing that at AST level duplicates it in every consumer;
  expressing it in LLVM IR loses the information needed to fuse the loops.
- `ON`-units and enable/disable prefixes are a control-flow *state* that must
  be reasoned about before it becomes landing pads.
- PL/I conversions form a lattice; inserting them as explicit HIR nodes makes
  them foldable by a single pass instead of being smeared through codegen.

HIR is therefore "PL/I with all implicit things made explicit"; MIR is "machine
independent PL/I-free code". MIR keeps us honest about what LLVM cannot know
(descriptor invariants, condition enable-state, string aliasing).

## 3. Components

| Component | Files (M0) | Responsibility |
|---|---|---|
| Driver | `src/main.cpp` | option parsing, phase sequencing, sub-process invocation |
| Diagnostics | `src/diag.{h,cpp}` | locations, severity, rule citations, caret output |
| Lexer | `src/lexer.{h,cpp}`, `src/token.h` | rules (130)–(151); comments; composite operators; not-symbol spellings |
| Parser | `src/parser.{h,cpp}` | rules (1)–(129); keyword recognition; multiple closure; recovery |
| AST | `src/ast.h` | syntax tree |
| Types | `src/types.h` | attribute → type mapping |
| Sema | `src/sema.{h,cpp}` | scopes, declarations, defaults, typing, conversions |
| IR generation | `src/irgen.{h,cpp}` | LLVM IR via `llvm::IRBuilder<>` (ADR-002) |
| Runtime | `runtime/pli_rt.{h,c}` | I/O, string semantics, conditions |

### 3.1 Source manager

Handles what PL/I inherited from punched cards and what modern users expect:

- Input encodings: UTF-8 (default), ISO-8859-1, EBCDIC (`--source-encoding`)
  with `¬ | ¢` mapped from their EBCDIC code points.
- Margins: free-form by default; `--margins=2,72` for card-image source, with
  the sequence-number field ignored.
- Character-set mode: 60-character (default) or 48-character
  (`--charset=48`), which enables the operator words `NOT AND OR GT LT GE LE
  NG NL NE CAT PT` as *reserved* words and the `..`/`:` substitutions of
  TR §2.3.3.
- `%INCLUDE` expansion with an include stack, so diagnostics report the
  inclusion chain.

### 3.2 Lexer

Produces `Word`, `Number`, `CharLit`, `BitLit`, punctuation and operators.
Two PL/I-specific obligations:

- **No keyword classification.** Keywords are recognised by the parser
  (ADR-004). The token stream for `IF IF = THEN THEN THEN = ELSE;` is nine
  words and two operators.
- **Composite operators are single words** (`**`, `||`, `>=`, `<=`, `¬=`,
  `¬>`, `¬<`, `->`), because TR §2.3.1 step 1 treats them as indivisible
  notation constants; the not-symbol is accepted as `¬`, `^` or `~`.

### 3.3 Parser

Recursive descent, one function per production group, plus precedence climbing
for expressions with the exact precedence of rules (115)–(122):

```
|  <  &  <  comparison  <  ||  <  + -  <  * /  <  ** / prefix + - ¬
```

Two features drive the design:

- **Positional keyword recognition** with bounded lookahead, then (M1)
  speculative parse plus symbol-table consultation — ADR-004.
- **Multiple closure** (TR §2.3.2.2): `END L;` closes every open block up to
  the one labelled `L`. Implemented by returning an `EndInfo` outward through
  the block-parsing functions until the owning block claims it.

Error recovery is statement-granular: a malformed statement is reported once
and the parser resynchronises on the next `;`, which suits a language whose
statements are unambiguously `;`-terminated.

### 3.4 Semantic analysis

Ordered sub-phases, because PL/I declarations are order-independent within a
block but attribute defaults depend on the complete attribute set:

1. **Block structure**: build the scope tree (external procedure, internal
   procedures, `BEGIN` blocks, `DO` groups do *not* introduce a scope).
2. **Declaration collection**: `DECLARE` (rule 9) including factored lists and
   level-numbered structures; `ENTRY`/`FILE`/label declarations; `LIKE`
   expansion; `DEFINED`/`iSUB` base resolution.
3. **Contextual declarations**: names that acquire attributes from context
   (a `BASED` locator, a `SET` target, an `OFFSET` reference) per Y33-6003.
4. **Implicit declarations**: undeclared identifiers get `FIXED BINARY` for
   initials I–N, otherwise `FLOAT DECIMAL` — implemented in M0, warned about,
   and suppressible with `--strict-declare`.
5. **Attribute defaults and conflict checking**: rules (14)–(43).
6. **Structure layout**: offsets per `ALIGNED`/`UNALIGNED`, with the mapping
   rules for structures containing varying strings and areas.
7. **Expression typing and conversion insertion**: the target-type algorithm
   for arithmetic (base/scale/mode/precision), string, and bit operands.
8. **Condition enable-state**: propagate prefixes (rules 60–63) down the
   statement tree so codegen knows which checks to emit.
9. **PICTURE compilation**: parse the picture string (rules 146–148) into a
   field program used by both edit-directed I/O and conversions.

### 3.5 Runtime interface (libpli)

The ABI is the set of `pli_*` symbols. M0 implements the shaded subset:

| Area | Entry points | Status |
|---|---|---|
| program start/stop | `pli_rt_init`, `pli_rt_fini`, `pli_stop` | M0 |
| list-directed output | `pli_put_skip`, `pli_put_page`, `pli_put_list_*` | M0 |
| string semantics | `pli_assign_char`, `pli_assign_varying`, `pli_concat`, `pli_cmp_char` | M0 |
| conversions | `pli_cvt_<from>_<to>` | M2 |
| decimal arithmetic | `pli_dec_add/sub/mul/div/cmp` | M2 |
| PICTURE | `pli_pic_edit`, `pli_pic_validate` | M2 |
| storage | `pli_area_alloc/free`, `pli_ctl_push/pop` | M4 |
| conditions | `pli_on_push/pop`, `pli_signal`, `pli_goto_nonlocal` | M5 |
| stream I/O | `pli_get_*`, `pli_put_edit_*`, `pli_open/close` | M6 |
| record I/O | `pli_read/write/rewrite/delete/locate` | M7 |
| tasking | `pli_task_create/wait/priority` | M9 |

## 4. Data representation and ABI

| PL/I data | Representation | Notes |
|---|---|---|
| `FIXED BINARY(p,0)`, p≤31 | `i32` | |
| `FIXED BINARY(p,0)`, p≤63 | `i64` | |
| `FIXED BINARY(p,q)` | integer scaled by 2^-q, scale static | |
| `FIXED DECIMAL(p,q)`, p≤18 | `i64` scaled by 10^-q | scale is a compile-time property (ADR-006) |
| `FIXED DECIMAL(p,q)`, p>18 | `i128`, else packed BCD in memory + runtime | |
| `FLOAT DECIMAL(p)` | `float` (p≤6), `double` (p≤16), `fp128` | `FLOAT BINARY` analogous on bits |
| `CHARACTER(n)` | `[n x i8]`, blank padded | |
| `CHARACTER(n) VARYING` | `{ i32 len, [n x i8] }` | current length prefix |
| `CHARACTER(*)` parameter | descriptor `{ ptr, i32 }` | |
| `BIT(n)` | packed, `[ceil(n/8) x i8]`; `BIT(1)` as `i8` (`i1` in registers) | |
| `PICTURE '…'` | `[n x i8]` + compiled field program | |
| `POINTER` | `ptr` | |
| `OFFSET(area)` | `i32` relative to the area's data | |
| `AREA(n)` | `{ i32 size, i32 free, [n x i8] }` + runtime allocator | |
| `LABEL` variable | `{ ptr code, ptr frame }` | frame enables non-local `GO TO` |
| `ENTRY` variable | `{ ptr code, ptr static_link }` | |
| `FILE` | `ptr` to runtime control block | |
| structure | LLVM struct; layout from the mapping rules | `UNALIGNED` packs bit/char |
| array | contiguous, row-major; dope vector when extents are dynamic | |
| `TASK`, `EVENT` | runtime handles | M9 |

**Parameter passing.** By reference, as PL/I requires: the callee receives
addresses. When an argument needs conversion, is an expression, or is a
constant, the caller materialises a **dummy argument** and passes its address
(M0 already implements this — `tests/core/procs.pli`). Aggregates with `*` extents
and `CHARACTER(*)` pass a descriptor. Internal procedures additionally receive
a static link (ADR-027) for access to the enclosing block's automatic storage.
All procedure variables are `AUTOMATIC` (`alloca`), so each activation owns its
own copy and external procedures are reentrant; a static link is one `ptr`
parameter per enclosing variable an internal procedure accesses.

**Name mangling.** `EXTERNAL` procedures and variables keep their upper-cased
PL/I name so that classic linkage and C interop work. Internal procedures are
`PLI_<outer>$<name>`; internal static variables are module-private
(`@pli_g_<name>` in M0).

**Entry point.** The `OPTIONS(MAIN)` procedure gets a C `main` shim that
initialises the runtime, calls the procedure, and runs normal termination
(which is where `FINISH` is raised, M5).

## 5. Condition handling (`ON` units)

PL/I conditions are dynamically scoped, re-entrant, and can be exited with a
non-local `GO TO` — the hardest part of the language to compile well.

- **Established handlers** live in a thread-local stack in libpli. Entering a
  block that contains `ON` statements pushes handler records; block exit pops
  them (`REVERT` pops a specific one). The stack is only touched by blocks that
  actually contain `ON`/`REVERT`, so the common path is free.
- **`ON`-units compile to functions** taking the establishing frame's pointer,
  so an on-unit can reference the variables of its block.
- **Computational conditions** (`FIXEDOVERFLOW`, `SIZE`, `ZERODIVIDE`,
  `CONVERSION`, `SUBSCRIPTRANGE`, `STRINGRANGE`) are *checks emitted inline
  only where the enable-state says they are enabled* (rules 60–63). Disabled
  checks cost nothing; enabled ones are ordinary branches that LLVM can hoist
  and merge.
- **`GO TO` out of an on-unit or block** unwinds to the target frame. We use
  LLVM's `invoke`/`landingpad` for cleanups and a runtime `pli_goto_nonlocal`
  that unwinds to the recorded frame; `LABEL` variables therefore carry a frame
  pointer.
- **Enable-state is compile-time knowledge**, so `(NOSUBSCRIPTRANGE): DO ...`
  is not a runtime flag test but the absence of code.

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
from the spec by `scripts/gen_rules.py`, so it cannot drift). Planned
additions: fix-it hints and `-fdiagnostics-format=json`.

## 7. Testing architecture

| Layer | Mechanism | Status |
|---|---|---|
| Lexer/parser units | golden token/AST dumps | M1 |
| IR golden tests | `plic -emit-llvm` + FileCheck-style matching | M1 |
| Execution tests | compile, run; diff (`expected/`) or PASS-grep (`tests/usecases/`) | M0 (13) |
| Diagnostic tests | `tests/*/bad_*.pli` must be rejected with the right rule | M0 |
| Conformance matrix | every rule (1)–(151) mapped to a test (GRAMMAR-COVERAGE.md) | M0 skeleton |
| Corpus compilation | compile `references/code/**` (Iron Spring, MULTICS, RosettaCode samples) | M2+ |
| Differential testing | run corpus outputs against another PL/I implementation | M3+ |
| Fuzzing | grammar-directed fuzzer over rules (1)–(151); parser must not crash | M2+ |

## 8. Current status (M0 wireframe)

Implemented end to end: `PROCEDURE`/`END` with multiple closure, internal
procedures with by-reference parameters and dummy arguments, `DECLARE` with
the arithmetic/string attribute defaults, implicit declarations, assignment,
`IF`/`THEN`/`ELSE`, `DO` groups (`DO;`, `DO WHILE`, iterative with `TO`/`BY`/
`WHILE`), `BEGIN` blocks, `CALL`, `RETURN`, `STOP`, `PUT [SKIP] [PAGE] LIST`,
the full operator set with spec precedence, 48-character-set operator words,
`CHARACTER` fixed/`VARYING` with concatenation and padded comparison, `BIT(1)`,
`FLOAT`, `FIXED BINARY/DECIMAL` with scale 0. M1 adds static links so internal
procedures reach enclosing automatic storage and external procedures are
reentrant (ADR-027), and `ENTRY` statements (ADR-026).

Everything else is diagnosed as unimplemented with its rule number, which is
also the project's to-do list: see IMPLEMENTATION-PLAN.md.
