# Design decisions

Numbered, immutable records. Each states the context, the decision, its
consequences, and what was rejected. "M*n*" refers to milestones in
IMPLEMENTATION-PLAN.md.

---

## ADR-001 — Implementation language: C++20

**Context.** The compiler must link against LLVM, whose stable API is C++.
**Decision.** C++20 for the compiler; C11 for the runtime library.
**Consequences.** Direct use of `llvm::IRBuilder`, LLVM's pass manager, and
MLIR later if wanted. The runtime stays C so it is trivially linkable into
foreign environments and has no C++ runtime dependency.
**Rejected.** Rust with `llvm-sys`/`inkwell` (FFI churn against a moving LLVM
C++ API, and the C API omits much of what we need); OCaml (excellent for the
front end, poor LLVM story); self-hosting in PL/I (attractive as a stunt,
disastrous for contributor supply).

---

## ADR-002 — LLVM interface: textual IR now, C++ API from M1

**Context.** M0 must build and run in an environment with `clang` but no LLVM
development libraries and no CMake.
**Decision.** M0 emits textual LLVM IR and invokes `clang` to
assemble/optimize/link. All emission goes through the `IRGen`/`Val` interface
in `src/irgen.h`.
**Consequences.** Zero build dependencies, and the emitted IR is human
readable, which is a real debugging asset early on (`plic -emit-llvm`). Costs:
no in-process verification, no access to LLVM analyses, string-formatting
overhead.
**Migration.** M1 re-implements the *bodies* of `IRGen`'s methods against
`llvm::IRBuilder<>` (`Val::reg` becomes `llvm::Value*`); parser and sema are
untouched. The M0 textual emitter is retained behind `--emit-textual-ir`
because it is useful in tests.
**Rejected.** Emitting C (loses precise control of overflow/EH); a bespoke
backend (decades of work); MLIR as the *only* level (valuable for the aggregate
dialect later, overkill for scalars now).

---

## ADR-003 — Hand-written lexer and parser

**Context.** PL/I is not LL(k) or LALR(k) in any comfortable sense: no reserved
words, statements distinguished only by their leading keyword *in context*,
declarations whose meaning depends on attribute sets, and an `IF(3)=5;` /
`IF (X)=1 THEN` ambiguity that needs unbounded lookahead or symbol knowledge.
**Decision.** Hand-written recursive-descent parser with precedence climbing.
**Consequences.** Full control over lookahead, speculation, and error recovery;
diagnostics can cite rule numbers; no generator in the build.
**Rejected.** Bison/ANTLR: both need lexer feedback hacks for keywords, and
their error recovery would be worse than statement-level resynchronisation for
a `;`-terminated language. GLR/Earley: general enough, but the ambiguity
resolution rules would still have to be written by hand, and performance and
diagnostics suffer.

---

## ADR-004 — Keyword recognition: positional, with bounded speculation

**Context.** TR 25.084 defines keywords as *notation constants*, not reserved
words. `DECLARE IF FIXED BINARY(31); IF = 1; IF IF = 1 THEN ...` is legal.
**Decision.** The lexer never classifies words. The parser applies, in order:

1. **Position.** Only in statement-initial position (after label prefixes and
   condition prefixes) is a word a candidate statement keyword; only in
   operator position are the 48-character-set words operators.
2. **Bounded lookahead** (implemented, M0). `WORD =` and `WORD ( … ) =` are
   assignments; anything else with a keyword spelling is that keyword.
3. **Speculative parse** (M1). Where 1–2 remain ambiguous, parse both
   alternatives on a saved token position and keep the one that succeeds;
   prefer the interpretation consistent with the symbol table (a declared
   variable named `IF` biases towards assignment).

**Consequences.** `tests/core/keywords.pli` compiles and runs, using `IF`, `THEN`,
`ELSE`, `DO`, `END`, `PUT` as variables. Keyword-position parsing is a single
predicate (`atStmtKeyword`), so the rule is stated once.
**Rejected.** Reserving keywords (rejects valid PL/I, and legacy code does use
`DATE`, `TIME`, `COUNT`, `LINE` as variables); lexer feedback from the symbol
table (fragile, and wrong for implicit declarations that do not exist yet).

---

## ADR-005 — Four-level IR: AST → HIR → MIR → LLVM IR

**Context.** PL/I semantics that LLVM cannot represent: aggregate expressions,
`BY NAME` assignment, conversion lattice, condition enable-state, descriptors,
`DEFINED`/`iSUB` overlays.
**Decision.** HIR = PL/I with everything implicit made explicit (conversions,
descriptors, on-unit establishment, loop semantics), still structured.
MIR = scalarised, PL/I-free, explicit checks and temporaries, CFG with EH
regions.
**Consequences.** Each optimization has an obvious home (OPTIMIZATION.md);
aggregate loops can be fused before LLVM sees them; conversions fold once.
Cost: two extra data structures and their maintenance.
**Rejected.** AST → LLVM directly (M0's approach; does not scale past scalars);
a single "mid" IR (either too abstract for checks or too low for aggregates).

---

## ADR-006 — Decimal arithmetic: statically scaled binary integers

**Context.** `FIXED DECIMAL(p,q)` is pervasive in real PL/I (money), and its
arithmetic has *exactly specified* result precision/scale rules. Y33-6003 also
defines a `FIXED` division whose result scale surprises newcomers
(`7/2 = 3.5`).
**Decision.** Represent `FIXED DECIMAL(p,q)` with p≤18 as an `i64` holding the
value scaled by 10^q, with p and q compile-time properties; p≤38 uses `i128`;
larger uses packed BCD with runtime helpers. Precision/scale of every operation
is computed at compile time; rescaling is explicit multiply/divide by powers of
ten. `SIZE`/`FIXEDOVERFLOW` checks compare against the declared precision.
**Consequences.** Decimal arithmetic becomes ordinary integer arithmetic that
LLVM optimizes well (constant folding, strength reduction, vectorisation),
while remaining exact. Division needs care: the result scale is derived from
the rules, then the quotient is computed with a widening multiply.
**M0 deviation.** Only scale 0 is implemented; scaled declarations are
diagnosed. `/` and `**` are evaluated in floating point, which reproduces the
observable result for the common `I = 7/2` case (truncation on assignment) but
is *not* the specified behaviour and is fixed in M2.
**Rejected.** Packed BCD everywhere (slow, defeats LLVM); binary floating point
for `DECIMAL` (wrong: 0.10 is not representable, and PL/I programs depend on
exact decimal); IBM zoned decimal in registers (no hardware support off-Z).

---

## ADR-007 — Varying strings: inline length prefix, descriptors at boundaries

**Context.** `CHARACTER(n) VARYING` has a maximum length fixed at declaration
and a current length that changes.
**Decision.** In storage: `{ i32 current_length, [n x i8] data }`. Across
parameter boundaries and for `CHARACTER(*)`: a descriptor `{ ptr, i32 }`.
**Consequences.** No heap traffic for the overwhelmingly common automatic case;
SROA promotes short strings into registers; the length is one load. Passing a
`VARYING` argument by reference exposes the same layout, as PL/I requires
(the callee may assign to it).
**Rejected.** Heap-allocated string objects (allocation per assignment,
lifetime problems with `BASED` storage); NUL termination (PL/I strings may
contain NUL, and length is O(1) information we already have).

---

## ADR-008 — Aggregates: static layout, descriptors only when necessary

**Context.** Arrays and structures may have constant or dynamic extents, may be
`ALIGNED`/`UNALIGNED`, and may be passed to procedures expecting `*` extents.
**Decision.** Compute layout statically whenever bounds and lengths are
constants; emit LLVM aggregate types so that LLVM's own analyses apply. Emit a
dope vector (bounds, multipliers, element descriptor) only for: dynamic
extents, `CONTROLLED`/`BASED` allocations with `*`, parameters declared with
`*`, and `LIKE`-cloned aggregates whose base is dynamic.
**Consequences.** The common `DECLARE A(100) FIXED BIN(31);` becomes
`[100 x i32]` with all subscripting visible to LLVM.
**Rejected.** Descriptors everywhere (uniform but pessimises the common case
and blocks vectorisation).

---

## ADR-009 — Conditions: runtime handler stack + compile-time enable-state

**Context.** `ON` units are dynamically scoped and can be left by a non-local
`GO TO`; prefixes enable/disable computational checks lexically.
**Decision.** Handler establishment is a runtime stack, touched only by blocks
containing `ON`/`REVERT`. Enable-state is resolved entirely at compile time and
governs whether a check is emitted. Non-local exits use LLVM
`invoke`/`landingpad` cleanups plus `pli_goto_nonlocal`; `LABEL` values carry a
frame pointer.
**Consequences.** Programs that establish no handlers pay nothing; disabled
checks vanish; the optimizer sees ordinary branches and can hoist them out of
loops.
**Rejected.** `setjmp`/`longjmp` per block (kills optimization across the
block); a flag word tested at each operation (defeats the point of lexical
prefixes); mapping every condition onto C++ exceptions (wrong semantics: PL/I
on-units *resume* by default rather than unwind).

---

## ADR-010 — Storage classes

**Context.** `AUTOMATIC`, `STATIC`, `CONTROLLED`, `BASED`, plus `AREA`.
**Decision.** `AUTOMATIC` → `alloca` (SROA-friendly). `STATIC` → LLVM globals,
`internal` unless `EXTERNAL`. `CONTROLLED` → runtime-managed generation stack
(`pli_ctl_push/pop`), since `ALLOCATE` on an already-allocated variable pushes
a new generation. `BASED` → no storage; references are `ptr` + offset
arithmetic. `AREA` → a runtime sub-allocator inside the area's bytes so that
`OFFSET` values stay valid when the area is assigned or written to a file.
**M0 deviation.** Variables of the *external* procedure are given static
storage so internal procedures can reach them without a static link. This is
observationally equivalent unless the external procedure recurses, which M0
does not support. M1 introduces static links and makes them `AUTOMATIC`.
**Rejected.** Making everything static (breaks `RECURSIVE`, tasking, and
reentrancy); heap-allocating automatic storage (needless cost).

---

## ADR-011 — Implicit and contextual declarations are implemented, not banned

**Context.** Y33-6003 declares undeclared identifiers implicitly (initial
letters I–N → `FIXED BINARY`, otherwise `FLOAT DECIMAL`). This is a notorious
source of bugs but is *required* to compile legacy code.
**Decision.** Implement faithfully; warn by default; provide
`--strict-declare` to make it an error.
**Consequences.** Legacy code compiles; new code can opt into safety. The
warning text names the attributes chosen, which is what a reader needs.
**Rejected.** Silence (unhelpful); hard error by default (rejects valid PL/I).

---

## ADR-012 — Multiple closure via outward propagation

**Context.** TR §2.3.2.2: `END L;` closes every block up to the one labelled
`L`; an unlabelled `END` closes the innermost.
**Decision.** Block-parsing functions return an `EndInfo`; a block that does
not own the label re-propagates it to its parent. A `pendingEnd_` slot carries
the information across statement-level returns.
**Consequences.** ~20 lines, no backtracking, and the error "END label 'X' does
not match any open block" falls out naturally. Verified by
`tests/core/loops.pli`, where one `END OUTER;` closes two nested groups.
**Rejected.** Post-hoc AST surgery (loses the diagnostic); requiring labels
(non-conforming).

---

## ADR-013 — Character sets: UTF-8 default, 48-char and EBCDIC as modes

**Context.** Source may arrive as EBCDIC card images using the 48-character
set, or as modern UTF-8 files.
**Decision.** Default: UTF-8, free-form, 60-character set, with `¬` accepted as
`¬`/`^`/`~`. `--charset=48` enables the operator words as reserved and the
substitution rules; `--source-encoding=ebcdic` and `--margins=m,n` handle card
images. Character *data* is bytes; `--data-encoding` selects the collating
sequence used by comparisons and `PICTURE` validation.
**Consequences.** The M0 lexer already accepts both not-symbol spellings and
the operator words in operator position (`tests/core/arith.pli`).
**Rejected.** UTF-8-only (cannot read the corpus); making the 48-character
words unconditionally reserved (would break 60-char programs using `OR` as a
variable).

---

## ADR-014 — `/` and `**` in M0 are floating point

**Context.** Exact `FIXED` division requires the precision/scale machinery of
ADR-006, which M0 does not have.
**Decision.** Evaluate `/` and `**` in `double` during M0; document it.
**Consequences.** `I = 7/2` yields 3 (truncation on assignment) and
`PUT LIST(7/2)` prints 3.5 — both match PL/I. Results diverge for values beyond
53 bits of significand and for exact decimal scales. M2 replaces this.
**Rejected.** Integer division (wrong: PL/I's `7/2` is 3.5, and
`PUT LIST(7/2)` must print 3.5); refusing to compile `/` (useless wireframe).

---

## ADR-015 — Statement-level error recovery

**Decision.** On a syntax error: report once with the rule number, skip to the
next `;`, continue. Sema errors do not stop analysis of sibling statements.
**Consequences.** One bad statement yields one diagnostic, not a cascade —
visible in `tests/core/bad_attrs.pli`, which reports five independent declaration
errors in one run.
**Rejected.** Panic-to-`END` (loses too much); error productions in a grammar
generator (n/a per ADR-003).

---

## ADR-016 — Multitasking deferred to M9, with a thread-based design

**Context.** `TASK`, `EVENT`, `PRIORITY`, `WAIT` (rules 79, 82, 94) are in the
1968 language.
**Decision.** Defer. When implemented: PL/I tasks map to OS threads;
`EVENT` variables to a condition-variable + state pair; `PRIORITY` to a
best-effort thread priority; the condition handler stack and `CONTROLLED`
generation stacks become thread-local; `ABNORMAL` data becomes `volatile`-like
(no cross-task caching).
**Consequences.** Everything the runtime does before M9 must be written so
these can be made thread-local without touching generated code.
**Rejected.** Green threads (would need our own scheduler and blocking-I/O
shims); dropping tasking (it is part of the specified language).

---

## ADR-017 — `PICTURE` is compiled, not interpreted

**Context.** A picture specification (rules 146–148) is a small program:
digit positions, zero suppression, drifting signs, insertion characters,
scaling factor, sterling fields.
**Decision.** Sema parses each distinct picture once into a *field program*;
codegen either calls a generic runtime interpreter (cold paths) or emits a
specialised routine (hot paths, monomorphic pictures).
**Consequences.** Edit-directed I/O and numeric-character assignment share one
implementation; common pictures like `'ZZZ,ZZ9.99'` become straight-line code.
**Rejected.** Interpreting the picture string at every use (slow and repeats
parsing); rejecting `PICTURE` (it is central to PL/I's business use).

---

## ADR-018 — `DEFINED` and `iSUB` overlays are address computations plus alias metadata

**Context.** `DEFINED` overlays one declaration on another's storage; `iSUB`
defining applies a subscript transformation (`DECLARE Y(5) DEFINED X(2*1SUB)`).
The `iSUB` dummy variable is a genuine part of the concrete syntax — rule (134),
`isub ::= integer SUB`.
**Decision.** Resolve the base reference in sema; represent a defined reference
as an address computation over the base; record the overlay in an alias-set
graph exported to LLVM as `!alias.scope`/`!noalias` metadata so the optimizer
knows the two names touch the same bytes.
**Consequences.** No copying, correct aliasing, and `iSUB` transformations
become ordinary index arithmetic that LLVM can simplify.
**Rejected.** Copy-in/copy-out (wrong semantics); treating everything as
may-alias (pessimises the whole procedure).

---

## ADR-019 — Diagnostics cite specification rules

**Decision.** Diagnostics carry `[TR 25.084 rule (n)]` where a production is
implicated, and Y33-6003 section names where semantics are.
**Consequences.** Reports are auditable against the spec; the "not implemented
in this stage" messages double as a machine-checkable to-do list — grep the
sources for rule numbers to find gaps.
**Rejected.** Opaque error codes (require a manual to decode).

---

## ADR-020 — Testing: execution tests plus a rule-coverage matrix

**Decision.** Every feature lands with (a) an execution test whose stdout is
diffed, (b) a diagnostic test if it can fail, (c) an entry in
GRAMMAR-COVERAGE.md. Later: IR golden tests, corpus compilation over
`references/code/**`, grammar-directed fuzzing, differential testing against
another PL/I implementation.
**Consequences.** Coverage is measured against the *specification*, not against
our own code.
**Rejected.** Unit tests alone (they would not catch semantic drift in
codegen); relying on the corpus alone (it does not exercise dark corners like
`iSUB` defining or multiple closure).

---

## ADR-021 — External C entries: `DECLARE … ENTRY` and by-reference calls

**Context.** PL/I must be able to call procedures written in C (architecture
goal 4: "interoperate with C ABIs"; `OPTIONS(BYVALUE)` / C-interop attributes
are M9). Rule (38) `ENTRY` was "not implemented in this stage".

**Decision.** `DECLARE name ENTRY ( t1, … tn )` declares an external entry: a
`ProcName` symbol with no PL/I body whose C symbol is resolved at link time.
Because the lexer uppercases identifiers but C names are case-sensitive (and
usually lowercase), the `EXTERNAL('symbol')` extended form spells the exact C
symbol (the z/OS PL/I ILC convention); without it the default is the
upper-cased identifier. Codegen emits a forward `declare` (one `ptr` per
parameter) and a `CALL` passes each argument **by reference** (the PL/I
default), so the C callee receives pointers — matching how an `ENTRY`
descriptor parameter is passed. A `-c` flag compiles a unit to a relocatable
object so a PL/I program can be linked against C objects (and a PL/I library
unit against a foreign `main`).

**Consequences.** PL/I can call C functions in a separate compilation unit,
linked together with `libpli` (test `tests/core/cinterop.{pli,c}` calls a
lowercase `c_set` via `EXTERNAL('c_set')`). `ENTRY` parameters may also be
passed as expressions via a dummy argument, like any PL/I by-reference call.
`RETURNS` (function values), full descriptors, and `USES`/`SETS` remain M2;
`OPTIONS(BYVALUE)` for value-passing is M9.
**Rejected.** Implicit external entries (any undeclared `CALL` target) — this
would mask typos and break the "diagnose, never silently accept" invariant;
keeping the `@PLI_` prefix on entry symbols (breaks the C symbol name match);
blindly lower-casing every imported symbol (would make an upper-case C
function unreachable).

## ADR-022 — Function procedures: scalar results by value

**Context.** M1's exit criterion is a recursive `FACTORIAL` function. Rule (34)
`RETURNS` was "not implemented in this stage". A function procedure needs a
calling convention for its result and a rule for what `RETURN(value)` means
(rule (81)).

**Decision.** `PROC [...] RETURNS(t)` marks a function procedure whose result
type `t` is the scalar computational type carried by its symbol and by
`Expr::Call`. The result is returned **by value in a register** of LLVM type
`t.llvmTy()` (`i32`/`i64`/`double`/`i8` for `BIT`); `RETURN(value)` converts the
expression to `t` and `ret`s it, and a function falling off its end returns a
zero value. Arguments stay **by reference** like any PL/I procedure, so a
recursive call passes a fresh dummy alloca for each by-value argument and reads
its parameter through the per-frame pointer — recursion needs no static link.
`RETURN(value)` is an error in a non-function procedure, and a function
procedure must contain a `RETURN(value)` (diagnosed, invariant 2).

**Consequences.** Recursive and mutually-referential scalar functions run
(`tests/core/func.pli`). Character-valued results are **not** yet supported —
they are diagnosed in codegen (ADR-021 kept them pending); a later milestone
will return strings via an sret descriptor or an out-parameter. By-value
parameters (`OPTIONS(BYVALUE)`) remain M9.

**Rejected.** Returning character values through the same register path (a
`char[n]` is not a first-class return type and would silently truncate); and
allowing a function to fall off without a value (would violate invariant 2).

## ADR-023 — `BEGIN` blocks are lexical scopes over a flat storage model

**Context.** Rule (68) `BEGIN` executed but shared the enclosing procedure's
scope: a name declared inside a block collided with an outer one ("already
declared in this block", rule (9)) and never leaked — but also could not
shadow. The plan assigns `BEGIN` "own scope in M1".

**Decision.** A `BEGIN` block opens a **child scope for name resolution only**:
names declared inside are visible in the block, shadow outer names of the same
spelling, and do not leak out. Storage, however, stays **flat** per ADR-010 —
block variables are still allocated at function entry (AUTOMATIC) or as module
globals (STATIC), not deallocated at block exit. Shadowed names therefore get
disambiguated `irName`s (a numeric `$N` suffix) so two same-spelled variables
map to distinct storage, resolved by lexical lookup.

**Consequences.** `tests/core/begin.pli` pins shadowing and outer-variable
preservation; an inner declaration no longer errors as a duplicate. There is no
run-time deallocation at block exit, so a block does not bound an automatic
variable's lifetime — a known simplification over full PL/I block activation
(M1's static-link work will revisit it). `INITIAL` on a nested-block variable
is still served by the existing static/automatic initialisation paths (flat
storage); block-entry initialisation of AUTOMATIC storage is deferred.

**Rejected.** Allocating (and deallocating) a fresh activation per `BEGIN`
block — the flat model keeps codegen simple and matches the M0 storage
simplification of ADR-010.

## ADR-024 — `SUBSTR` as a pseudo-variable is a runtime write into the source string

**Context.** Rule (86) allows a *reference* on the left of `=`. The M2 plan
lists `SUBSTR` as a pseudo-variable, i.e. `SUBSTR(v, i, n) = x` must overwrite
part of the character variable `v` in place.

**Decision.** The parser already routes `WORD ( … ) =` to an assignment (its
`looksLikeAssignment` lookahead), so no parse change is needed. Sema restricts
the target to a SUBSTR call whose first argument is a modifiable character
variable. Codegen lowers the pseudo-variable to a runtime call
`pli_substr_assign(dst, dstcap, start, len, src, srclen)` that writes up to
`len` characters of the RHS into `dst` at the 1-based position, blank-filling
the tail when the RHS is shorter.

**Consequences.** Only the fixed-length `CHARACTER` and (via the shared data
pointer) `VARYING` forms are served; substring bounds past the end of the
variable clip rather than raise `SUBSCRIPTRANGE` (that is M3). `SUBSTR` on the
right of `=` is unaffected — it is the ordinary built-in.

**Rejected.** Building a general pseudo-variable infrastructure (other
pseudo-variables, or targets that are arbitrary expressions) — KISS, one
pseudo-variable now; the runtime call keeps codegen simple.

## ADR-025 — A program-level scope makes sibling external procedures callable

**Context.** Pass 1 of sema declared a procedure's name only in its *parent's*
scope (`if (outer)`), so a name at the top level was never registered: external
procedures could not call each other, only nested ones could. This blocks the
entry-namelist feature (rule (3)) — a two-entry-point external procedure is
unusable if the MAIN procedure cannot reach it.

**Decision.** Introduce a single program-level `rootScope_`. Every external
procedure (no parent) is declared there, and every external procedure's own
scope takes `rootScope_` as its parent. Nested procedures are unchanged: they
stay in their parent's scope and can still reach the root through the chain.
Each entry-namelist name (rule (3)) is declared in the same scope as its
procedure, resolving to the same `Proc`; codegen emits an internal LLVM alias
per extra name so the symbol exists.

**Consequences.** Sibling external procedures can call each other — correct
PL/I behaviour, previously missing. `multientry.pli` pins a two-entry-point
procedure callable by both names from MAIN. All existing tests still pass;
lookup order (own scope before parent before root) preserves shadowing.

**Rejected.** Keeping external procedures mutually invisible (blocks the M1
exit criterion); or emitting one function body per entry name with no shared
scope (duplicates logic and still cannot be called from a sibling).


