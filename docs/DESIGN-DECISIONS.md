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

## ADR-026 — `ENTRY` statements: a shared impl split into entry-point segments

**Context.** The `ENTRY` statement (rule (56)) `label: ENTRY (params)
[RETURNS(type)]` declares an alternate entry point into a procedure, with its
own parameters and result type; a call through that name starts executing at
the `ENTRY` and continues to the end of the body. Naive codegen — one LLVM
function per entry point — cannot share the procedure's locals across entries,
since LLVM cannot jump between functions.

**Decision.** When a procedure contains any `ENTRY` statement, emit one shared
implementation function `@PLI_<name>.impl` carrying the whole body, split into
segments (the procedure's start, then one segment per `ENTRY` in order). The
impl takes the *union* of every entry point's parameters (by-reference `ptr`,
deduplicated by symbol) plus an `i64` entry selector; its entry block switches
on the selector to the matching segment, and segments fall through in body
order. Each entry name (the procedure's own names and each `ENTRY` label) is a
small thunk with that entry point's exact signature that tail-calls the impl,
forwarding its own arguments and `undef` for the other union slots, then
returns the result. Entry-namelist aliases (rule (3)) point at the procedure's
segment-0 thunk. A `CALL`/function reference to an `ENTRY` name resolves to
that name's thunk and its parameters.

**Consequences.** Locals stay as allocas in the impl and are shared across
segments; each entry point has its own calling convention; every existing test
still passes. `entry.pli` pins a function with two entry points reachable
through either name. Two limitations are diagnosed rather than silently
accepted: the impl returns one type, so a mixed return type across entry
points is rejected (rule (56)); a segment that reads a parameter not supplied
on its entry path sees `undef` — the PL/I "unused parameter" case, left to the
programmer.

**Rejected.** One LLVM function per entry chained by tail calls (locals would
need promotion to globals or threaded pointers); or one function per entry
with no shared body (duplicates logic and cannot share storage).

## ADR-027 — Static links make internal procedures reach enclosing automatic storage

**Context.** ADR-010's M0 deviation gave external-procedure variables static
(global) storage so internal procedures could reach them without a static
link. That is observationally equivalent only when the external procedure
never recurses: with shared globals, a recursive external procedure's internal
procedures read the *deepest* activation's copy of an enclosing variable, not
their own — reproduced by a recursive function whose internal helper reads an
enclosing `v` after the recursion, returning 0 instead of the correct sum.

**Decision.** Remove the deviation: every procedure's variables are now
`AUTOMATIC` (LLVM `alloca`), so each activation owns its own copy and external
procedures are reentrant. An internal procedure that (directly or through its
own internal procedures) references a variable of an enclosing procedure is
given one *static-link* parameter per such variable — a pointer to that
variable in the enclosing activation. The caller supplies a link as its own
address when it owns the variable, or re-threads its own link for a variable
owned higher up. `IRGen::addressOf` routes a reference to an enclosing variable
through the link. The environment each procedure needs is computed bottom-up
in sema (`Proc::env`). Links are appended after the ordinary parameters in the
signature and after the arguments at every call site (CALL, function
references, and the `ENTRY`-statement impl/thunks).

**Consequences.** Reentrant external procedures with internal helpers behave
per spec (`tests/core/staticlink.pli`); `INITIAL` on these now-automatic
variables is a per-activation prologue store, so the STATIC global path's
type-aware literal handling moved into `IRGen::emitInitials`. A procedure that
has no enclosing variables gains no link — zero overhead. One case is
diagnosed unimplemented rather than silently miscompiled: a "cousin" call in
which a procedure reaches a variable owned by a non-adjacent procedure between
itself and the callee (rule (8)).

**Rejected.** A display (an array of frame pointers) — more machinery than the
M1 case needs; promoting enclosing variables to globals again (reintroduces the
reentrancy bug); heap-allocating automatic storage (needless cost).

## ADR-028 — `--explain`: rule productions generated from the spec

**Context.** Diagnostics cite TR 25.084 rule numbers; a user wants to look up a
cited rule without opening the spec. The productions live only in
`TR25.084-concrete-syntax.md`, and AGENTS.md forbids duplicating spec text into
the codebase by hand.
**Decision.** `--explain <rule>` prints a production's formal text from a C++
table that `scripts/gen_rules.py` extracts from the spec at build time
(`build/rules.cpp`). The spec stays the single source of truth; a regeneration
cannot drift from it. Only the formal grammar lines inside the code fences are
extracted; the surrounding prose (including ⚠ OCR-caveat notes) is left in the
spec, so `--explain` points the user at the rule without re-stating the prose.
**Consequences.** `make` needs `python3` and the spec present at build time
(both already true in-repo). Driver tests (`tests/driver/*.sh`) were added to
`run_tests.sh` to cover a driver-level feature the `.pli` harness cannot.
**Rejected.** Hand-maintaining a rule table (duplicates the spec, drifts);
reading the spec file at runtime (requires the spec on the installed system,
and couples the binary to a repo path).

## ADR-029 — HIR as a field-for-field AST mirror plus explicit `Convert` nodes

**Context.** ADR-005 places an HIR between sema and codegen; the M1 plan asks
for it "initially as a thin mirror of the AST, plus `--print-hir`", and a later
decision adds explicit conversions. Sema annotates the AST in place (types,
symbols), so the lowering runs after sema and before IRGen.
**Decision.** HIR (`src/hir.h`) mirrors the AST node-for-node (same kinds and
fields), lowering (`src/hir.cpp`) copies the typed AST into it, and IRGen
consumes HIR. A new `Convert` node marks every implicit scalar conversion that
IRGen would otherwise apply inline (assignment, return, DO bounds, call args,
and the MIN/MAX/MOD/MULTIPLY/DIVIDE/PRECISION/ROUND built-in operands); `Convert`
is emitted by `emitExpr` through the existing conversion helper. Symbols keep
pointing at the still-alive AST procedures/entries — names mirror exactly, so
callee resolution is unchanged; each `HProc` carries a `src` back-pointer for
the rare AST↔HIR owner comparison.
**Consequences.** `plic --print-hir` shows the lowered, conversion-explicit
program; runtime behaviour is unchanged (a Convert wraps exactly where IRGen
already converted). Lowering mutates no AST. Cost: a parallel node set and its
maintenance.
**Rejected.** Making HIR a distinct, more abstract IR now (that is the MIR
milestone, M8); rewriting sema to build HIR directly (far beyond the M1 "thin
mirror" scope); dropping the mirror and keeping IRGen on the AST (leaves no
inspectable HIR for later passes to hang off).

## ADR-030 — Fix-its: insertions threaded through `Diags`, rendered at the caret

**Context.** The M1 diagnostics scope is "fix-its, `--explain <rule>`".
`--explain` is ADR-028. A fix-it should tell the reader what to type to repair
the error. Most syntax-recovery cases in this parser are *insertions* of a
missing token at the current token's position — `expected THEN`, `expected '='
in assignment statement`, `expected PROCEDURE after entry name`, and the EOF
case `missing END for '<name>'`.

**Decision.** `Diags::error`/`warn` gain one optional `replace` argument: the
text to insert at the diagnostic's location. `Diags::emit` renders it on a line
below the caret, padded to the same column, so the suggested text sits exactly
where it goes (clang-style "insert here"). Only unambiguous single-token
insertions carry a fix-it; a fix-it is only rendered where a real source line
exists (the EOF `missing END` case is guarded the same way the caret is).
Muted speculative-parse probes (ADR-004 step 3) never render fix-its because
they return before `emit`.

**Consequences.** `tests/driver/fixit.sh` checks that the four recovery cases
render their suggested token. Fix-its stay one-argument-simple; there is no
general replacement-span or multi-edit machinery yet. `-fdiagnostics-format=json`
(ARCHITECTURE §6) remains planned.
**Rejected.** A structured fix-it object with spans and kinds (a replacement of
an existing range, or a note attached to a *different* location) — more
machinery than the current recovery cases need; building it now would be
speculative (KISS).





## ADR-031 — IR golden tests: a dependency-free FileCheck-style matcher

**Context.** M1's exit criterion is "IR golden tests in place", and ARCHITECTURE
§7 lists them at M1, but no harness existed. A golden *diff* of emitted IR is
brittle — LLVM IR changes with version and optimization level. The plan names
"FileCheck-style matching" (ordered pattern checks, not whole-file diffs). The
LLVM install ships a `FileCheck` binary, but relying on it adds a test-time
dependency whose exact path is LLVM-version-specific (like `clang`).

**Decision.** A new `tests/ir/` group: each `<name>.pli` is compiled with
`plic -emit-llvm -o <out>/<name>.ll`, and a companion `<name>.check` holds
`CHECK: <regex>` directives matched **in order** against the IR by a minimal
matcher (`ir_match` in `tests/run_tests.py`). The matcher is dependency-free
(Python `re`); a FileCheck regex block `{{...}}` becomes a Python group
`(?:...)`, and the rest of the line is a Python regex. A new `ir` test kind in
the runner emits IR and applies the checks. `tests/ir/init.pli`, `arith.pli`,
`func.pli` (the recursive-factorial exit criterion) and `ifelse.pli` pin the
initial-value store, integer add/sub, signed compare + branch, and recursion.

**Consequences.** Codegen that silently drops work (e.g. an INITIAL never
reaching a store — the CONTRIBUTING worked example) fails the suite. No new
build or test dependency; the `.check` files double as documentation of the
expected IR shape. M1's exit criterion is now met.
**Rejected.** Whole-file IR diffs against `expected/*.ll` (brittle across LLVM
versions); invoking the `FileCheck` binary (an extra, version-pinned dependency
for what a few dozen lines of Python do).

---

## ADR-032 — Single source of truth for the pli_* runtime ABI

**Context.** Every runtime entry point's signature was hand-written twice: as a
C prototype in `runtime/pli_rt.h` and as an LLVM function type at each
`runtimeFn(...)` call site in `src/irgen.cpp`. A change to one side (a new
parameter, a different return type) could silently drift from the other,
producing IR that mismatches the C ABI at link time.

**Decision.** One table, `runtime/pli_rt_abi.def`, is the sole home of each
`pli_*` signature, written as type tokens (`VOID I64 I32 I8 DOUBLE PTR CPTR`).
Two consumers expand it by `#include` with redefined macros:
`runtime/pli_rt.h` maps the tokens to C types and emits prototypes;
`src/irgen.cpp` maps them to LLVM types and builds declarations. A function
with no parameters is written with a single `VOID` argument, which each
consumer drops. `irgen.cpp`'s `runtimeFn(name)` now looks up the signature
from the table instead of taking it from the caller, and `intrinsicFn(name,
ret, args)` is the narrow escape hatch for non-ABI LLVM builtins
(`llvm.pow.f64`, `llvm.fabs.f64`). The Makefile's `-MMD -MP` on the runtime
objects records the `.def` as a header dependency, so a signature change there
rebuilds both the C runtime and the code generator.

**Consequences.** The C ABI and the emitted IR cannot drift: one edit in the
`.def` propagates to both. `runtimeFn` call sites lose the error-prone explicit
signature. `pli_*` symbols not yet emitted (e.g. `pli_signal_error` for M5) live
in the table ahead of use, keeping it the complete ABI contract.
**Rejected.** Keeping two hand-written lists (drift, the original problem);
deriving one side from the other in a build script (a third source of truth for
the same information); having irgen read the `.def` at runtime (overkill — the
macro-expansion gives compile-time checking with zero cost).

---

## ADR-033 — Fixed-size arrays: static layout, scalar elements, SUBSCRIPTRANGE interim

**Context.** M2 begins with arrays (rules (12),(13),(126)). ADR-008 already
fixes the representation: constant bounds become an LLVM aggregate
(`DECLARE A(100) FIXED BIN(31);` → `[100 x i32]`), with dope vectors only when
bounds are dynamic. The first slice must decide how to lay out that aggregate,
which element types it serves, and how out-of-range subscripts are handled
before condition handling (M4) provides `ON SUBSCRIPTRANGE`.

**Decision.** A fixed-size array is stored as `[N x elemTy]` where `N` is the
upper bound and `elemTy` the element's scalar LLVM type (per ADR-008); the lower
bound defaults to 1 (a `(lb:ub)` bound pair is honoured). One dimension per axis
is served in this slice — a multi-axis declaration is diagnosed as rule (13).
Only scalar (numeric and `BIT(1)`) element types are served; a character-element
array is diagnosed as rule (12), never silently miscompiled (invariant 2).
`INITIAL` on an array (iteration factors) is diagnosed as rule (26). A
subscripted reference `A(i)` (rule (126)) lowers to a GEP with index `i - lb`;
a constant subscript is range-checked at compile time, and a runtime index is
guarded by an emitted check that calls a new `pli_subscript_oob` runtime helper,
which prints a `SUBSCRIPTRANGE` message and exits. The interim is a hard abort
because conditions are M4; ADR-009's handler mechanism replaces it when `ON
SUBSCRIPTRANGE` is implemented.

**Consequences.** Common vector/table programs compile and run with bounds
checking on (the M2 exit criterion). Out-of-range constant subscripts are caught
at compile time; out-of-range runtime subscripts stop the program deterministically
instead of corrupting memory. The dope-vector path (ADR-008) is deferred to
dynamic bounds and `*` extents. **Rejected.** Dope vectors for this slice
(unneeded for constant bounds); servicing character element arrays before the
string-addressing machinery is wired (incomplete and unsafe); silently skipping
bounds checks to gain speed before condition handling exists (invariant 2).

---

## ADR-034 — Array attribute built-ins `LBOUND`/`HBOUND`/`DIM`: compile-time constants

**Context.** ADR-033 serves fixed-size single-axis scalar arrays (rules
(12),(13),(126)). The next M2 increment (IMPLEMENTATION-PLAN) is the common array
built-ins `SUM`, `PROD`, `ANY`, `ALL`, `DIM`, `LBOUND`, `HBOUND`. The cheapest,
most broadly useful of these are the three attribute built-ins, because with
constant bounds they are pure compile-time values and they make bounds-driven
loops (`DO i = LBOUND(a) TO HBOUND(a)`) the idiomatic way to walk an array — the
pattern matrix/table programs depend on.

**Decision.** `LBOUND(a)` yields the declared lower bound (1 by default),
`HBOUND(a)` the upper bound, and `DIM(a)` the extent `ub - lb + 1`. Each takes a
single unsubscripted array argument (rule (127)) and, because bounds are
constants in this stage, folds in IRGen to an integer constant of the result
type's width (`FIXED BIN(31)`, an `i32`). A non-array argument, an extra argument
(incl. an axis number — only axis 1 exists), or a multi-axis array is diagnosed
as rule (123). The reduction built-ins (`SUM`/`PROD`/`ANY`/`ALL`) need a runtime
loop over the elements and are deferred to a later M2 slice.

**Consequences.** Bounds-driven loops compile to constant comparisons and work
with the existing `SUBSCRIPTRANGE` checks. The result constant must use the
result type's LLVM width — returning a 64-bit register for a 32-bit `FIXED
BIN(31)` result broke DO-loop bound storage (the TO bound was stored as `i64`
into an `i32` slot), so the value is built with `ConstantInt::get(llvmTy(e->ty))`
matching ADR-033's `[N x elemTy]` element width. **Rejected.** Returning the
bounds as 64-bit registers (width mismatch against `FIXED BIN(31)` storage);
implementing the reduction built-ins before the array-iteration machinery they
need; diagnosing the attribute built-ins as unimplemented when they are trivial
constants that directly enable the M2 exit criterion.

---

## ADR-035 — Array reduction built-ins `SUM`/`PROD`/`ANY`/`ALL`: an emitted loop

**Context.** ADR-034 deferred the reduction built-ins pending the array-iteration
machinery they need. ADR-033 lays arrays out as `[N x elemTy]`; this slice adds
`SUM`/`PROD` over numeric arrays and `ANY`/`ALL` over `BIT` arrays, walking the
whole single-axis extent (rule (123)). The open design question is how a reduction
gets the array elements: emit a loop in IRGen, or call a runtime helper.

**Decision.** Each reduction is lowered in IRGen to an LLVM loop over the extent
`0..N-1`: an entry alloca holds the accumulator (identity 0 for `SUM`, 1 for
`PROD`, `false` for `ANY`, `true` for `ALL`) and an i64 counter; each iteration
GEPs `base[0, i]`, loads the element, and combines it (`add`/`mul` for numeric —
float or integer per element type — `or`/`and` for bits). The result type is the
element type for `SUM`/`PROD` and `BIT(1)` for `ANY`/`ALL`. A non-array argument,
an unsupported element type (a `CHAR` or non-numeric array for `SUM`/`PROD`, a
non-`BIT` array for `ANY`/`ALL`), or an extra argument is diagnosed as rule (123).
The loop needs no `SUBSCRIPTRANGE` check because the walk stays within the extent.

**Consequences.** Common aggregate queries (`SUM`, `PROD`, `ANY`, `ALL` over a
vector or table) compile and run; the emitted loop reuses the existing
`addressOf` path, so arrays in an enclosing procedure's frame (static link) and
non-1 lower bounds are handled. The bit accumulator is held as an `i1` (matching
how `Val` represents `BIT` values) rather than the `i8` storage type. **Rejected.**
A per-element-type runtime helper (e.g. `pli_sum_i32`), because the arithmetic is
trivial in LLVM and a helper would fragment the ABI surface (ADR-032) and need
one entry per element type/operation; a `SUBSCRIPTRANGE` check on every iteration
(always in-bounds — pure overhead); and walking the array through the public
`A(i)` load (which would re-run bounds checks and be needlessly indirect).

## ADR-036 — Multi-axis fixed-size arrays: flat row-major layout

**Context.** ADR-033 served fixed-size **single-axis** scalar arrays and diagnosed
a multi-axis declaration as rule (13); ADR-034/035 read only `dims[0]` for the
bounds/reduction built-ins. The M2 plan (IMPLEMENTATION-PLAN, rules (12),(13),(126))
next needs matrix/table programs, which are 2-D and up. The open questions are how
to lay out a multi-axis array in the already-fixed `[N x elemTy]` aggregate and how
the bounds and reduction built-ins report a multi-axis array.

**Decision.** A multi-axis fixed-size array is stored in the same flat `[N x
elemTy]` aggregate with `N` the product of all axis extents, in **row-major**
order: the last axis is contiguous, and each earlier axis strides by the product
of the extents of the axes after it. A subscripted reference `A(i,j,...)` (rule
(126)) emits a per-axis `SUBSCRIPTRANGE` check (all flags OR-ed into one branch,
one `pli_subscript_oob` call) and computes the flat offset
`Σ_k (i_k − lb_k) · stride_k`, a single `GEP [0, offset]` into the aggregate. The
arity check in sema now requires one subscript per axis (rule (126)). The attribute
built-ins (ADR-034) keep their no-axis-argument form: `LBOUND`/`HBOUND` report the
**first** dimension (PL/I semantics for the single-argument form), while `DIM(a)`
now reports the total element count (the product of extents) — which is also what
the reduction built-ins (ADR-035) walk, so `SUM`/`PROD`/`ANY`/`ALL` and `DIM`
automatically span the whole array.

**Consequences.** Matrix and table programs compile and run with per-axis bounds
checking on (the M2 exit criterion), reusing the existing aggregate layout and the
`[N x elemTy]` element addressing. `DIM(a)` and the reductions are correct over
every axis with no change to their emitted loop. **Rejected.** A nested/GEP-per-axis
layout or a separate dope vector (neither needed for constant bounds — ADR-008
reserves dope vectors for dynamic extents and `*`); returning per-axis arrays for
the built-ins (out of scope until the axis-argument form `LBOUND(a,d)` is added);
and column-major order (PL/I is row-major). Dynamic bounds, `*` extents, and
cross-sections remain diagnosed.

## ADR-037 — Level-numbered structures: literal LLVM structs, members without symbols

**Context.** The M2 plan (rules (11),(124),(125)) needs `DECLARE 1 S, 2 A ..., 2 B ...`
— a level-numbered structure whose members are reached by qualified names like
`S.A`. A member has no storage of its own; the structure owns the aggregate, and
a qualified reference is a projection of it. Open questions are how to represent
the aggregate type and how sema/IRGen should resolve a qualification chain.

**Decision.** A structure compiles to an LLVM **literal struct** type laid out in
declaration order, recursing for nested (minor) structures. In sema,
`collectDecls` builds the level-numbered hierarchy (an item belongs to the nearest
preceding item with a strictly smaller level) into a `Type` of kind `Struct`; only
top-level items become symbols, and members get **no** standalone symbol. A
qualified reference `S.A.B` is resolved at sema time against the structure type,
recording the LLVM field index of each step in `memberPath`. IRGen descends with
one `CreateStructGEP` per field index, then loads/stores the leaf scalar. Whole
structures are **not** served this stage: using one as a value or as an
assignment target, or writing `INITIAL` on a structure, is diagnosed (rules
(26),(127)).

**Consequences.** Member qualification is a compile-time constant field index —
no descriptors, no runtime name lookup — and the layout is visible to LLVM's
analyses (per ADR-008). `CHARACTER` members are diagnosed pending varying-string
addressing. **Rejected.** Giving every member its own symbol/storage (breaks PL/I
semantics and complicates qualification); a per-member dope vector or descriptor
(unneeded for constant layout); serving whole-structure values in this stage
(diagnosed per unimplemented ≠ accepted).

## ADR-038 — Structure array members: `llvmTy` handles arrays, subscripted via memberPath

**Context.** Record-processing programs (the M2 exit criterion) declare array
fields inside structures, `2 A(10) FIXED BIN(31)`, referenced as `S.A(i)` (rules
(11),(124),(126)). A member is a `Type` with `dims`; the question is how to lay
it out and address a subscripted member array, and how a member array interacts
with the existing flat `[N x elemTy]` array codegen.

**Decision.** `llvmTy` now handles `isArray()` and emits `[N x elemTy]` (before,
only top-level arrays got their aggregate type via `addressOf`/`emitGlobals`
special-casing, so a member array collapsed to its scalar — a latent layout bug
that LLVM `-O2`/SROA miscompiled into `poison` for nested member arrays). A
member array therefore lays out as an `[N x elemTy]` field of the struct. In
sema, `S.A(i)` — which the parser sees as a Call with `path` — is reclassified
as a `Subscript` carrying the base structure symbol and the resolved
`memberPath`; arity and constant bounds are checked against the member's `dims`.
IRGen composes the two existing addressing schemes: `memberAddr` descends the
field indices to the member array's address, then `arrayElementAddr(arr, base,
…)` does the flat row-major GEP with the per-axis `SUBSCRIPTRANGE` check.
`memberType` mirrors `memberAddr` (no GEPs) to recover the leaf array type.

**Consequences.** Structure array members — nested (minor-struct) and multi-axis
— work in read/write/expression positions with the same bounds checking as
standalone arrays, reusing the existing aggregate layout (ADR-036/037).
**Rejected.** Giving array members their own symbol/allocation (breaks PL/I
member semantics); a dope vector for constant-bounds member arrays (unneeded —
ADR-008); re-deriving the member type by walking in IRGen instead of the single
`memberType` helper (duplication). `CHARACTER` array members remain diagnosed.

## ADR-039 — Array procedure parameters: by-reference addressing, widened sema checks

**Context.** The M2 exit criterion needs record/matrix programs to "pass arrays
and structures between procedures." Structures already pass by reference (a
parameter symbol's address is the passed pointer, ADR-037). Arrays did not: a
parameter array `X(I)` inside a callee errored because sema's Call→Subscript
reclassification only accepted `Symbol::Var`, and the array attribute/reduction
built-ins (`DIM`/`LBOUND`/`HBOUND`/`SUM`/`PROD`/`ANY`/`ALL`) did likewise.

**Decision.** Parameter arrays reuse the existing by-reference parameter
addressing: a callee's `addressOf(paramArray)` is the caller's array base
pointer, so subscripting and the array built-ins work unchanged once sema accepts
`Symbol::Param` wherever it accepted `Symbol::Var` for an array. No descriptor is
introduced for fixed, constant-bounds arrays; the callee checks bounds against
its own declared extents (ADR-036's flat `[N x elemTy]` layout and per-axis
`SUBSCRIPTRANGE` apply unchanged). Sema now accepts `Var` or `Param` in the
Call→Subscript reclassification and in the array attribute/reduction built-ins.
The parameter's static-link handling is unchanged: `resolveParams` sets a param's
owner to the current procedure, so a param is never treated as an enclosing
variable.

**Consequences.** Fixed-size arrays pass by reference into procedures and are
subscriptable on both sides of an assignment, multi-axis, and through the array
built-ins — with the same bounds checking as standalone arrays, satisfying the
M2 "pass arrays between procedures" criterion for constant bounds. **Rejected.**
A dope-vector descriptor for constant-bounds parameter arrays (unneeded — ADR-008
reserves descriptors for dynamic extents/`*`); copying array arguments by value
(PL/I is by reference); adding extent/shape checking between caller and callee
declarations in this stage (a later hardening step).

## ADR-040 — Whole-structure assignment: a storage copy between identical shapes

**Context.** The M2 scope ("aggregate ... assignment", rule (127)) and the
record-processing exit criterion need `S = T` — copying one structure's value
into another. Structures are literal LLVM structs with constant layout (ADR-037),
and only scalar/member-array assignment was served; a whole-structure assignment
was diagnosed. The open questions are what shapes are accepted and how to emit
the copy.

**Decision.** `S = T` is a whole-storage `memcpy` from the source structure's
address to the target's, of the target's LLVM struct store size. Sema accepts
only structures of **identical** `Type` (same member names and types, recursively
— a conservative by-position approximation), and diagnoses a shape mismatch or
mixing a structure with a non-structure (rule (127)). Either side may be a
top-level variable or a qualified member (`S.X = T.X`), resolved via `addressOf`
or `memberAddr`. To compute the copy size correctly the module is given a data
layout string derived from the target triple's pointer width (the OS mangling
does not affect type sizes).

**Consequences.** Record copying — including array members and nested minor
structures — works for identical shapes, reusing the existing literal-struct
layout and member addressing (ADR-037/038). Self-assignment is a harmless
no-op copy. **Rejected.** A by-position shape check that ignores member names
(real PL/I unqualified-assignment semantics; deferred until a mismatch-reporting
stage); whole-structure values in expressions and as return values (out of
scope — rule (127) keeps those diagnosed); computing the byte size by walking
members (wrong once a nested struct's padding is accounted for — the data layout
gives the true store size).

## ADR-041 — Multiple assignment: one shared RHS, same-type targets

**Context.** Rule (86) is `assignment-statement ::= {,• reference•••} = expression
[ , BY NAME ] ;` — a comma-separated list of targets all receive the value of the
single RHS. The M2 scope names "multiple assignment." The open questions are how
the shared RHS is evaluated and how per-target type conversion behaves.

**Decision.** `a, b, c = e` evaluates the RHS **once** and stores the resulting
value to every target through the existing scalar store paths (`storeTo`,
`storeArrayElement`, member stores), which each convert as needed. Because the HIR
lowers the value to the first target's type, sema requires every target to share
the **first target's type** and rejects SUBSTR and whole-structure targets inside a
multiple list (rule (86)); single-target assignment keeps its full behaviour
unchanged. The trailing `, BY NAME` suffix is diagnosed as unimplemented (BY NAME
is a separate M2 item).

**Consequences.** Scalar variables, array elements, and scalar structure members in
any mix store the same value, so `X(1), X(2), X(3) = 4` and `a, X(1), b = 9` work.
**Rejected.** Re-evaluating the RHS per target (wrong for a side-effecting
function-call RHS); per-target independent HIR value conversion (would re-derive
conversion once per target and require the RHS to be evaluated N times, and would
change the HIR/IR shape of the existing single-target case); serving mixed-type or
`BY NAME` multiple assignment in this stage.

## ADR-042 — Build tooling: clang stays the link driver; `make check` is the analysis gate

**Context.** The driver shells out to clang to assemble/optimize/link the emitted
IR (ADR-002); there is no other linker in the picture, and the repo wants a single
gate the agents run for major edits so static analysis is actually exercised.

**Decision.** `plic` keeps clang as its link driver and gains explicit link
controls (`-L`, `-l`, `-Wl`, `--linker`, `-shared`, `-static`, `--extra`) passed
through to that driver — no direct lld invocation. On the build side, `make check`
rolls the analyzers into one gate: a `-Werror` rebuild (`make werror`), a
clang-format drift check (`make fmt-check`, with `make fmt` to normalize),
clang-tidy over the C++ sources (`make tidy`, scoped to bug-catching checks), and
the clang static analyzer (`make scan`). `src/` is clang-format-normalized so the
gate is green from the start.

**Consequences.** The Makefile is the canonical home for build tooling; CMake stays
secondary and its `ctest` now drives `tests/run_tests.py` (fixing a dangling
`run_tests.sh` reference). New edits should pass `make check`. The one-time
reformat of `src/` is mechanical and covered by the full test suite.

**Rejected.** Driving lld directly (platform-specific sysroot/library matching on
macOS, and no bare `lld` installed); making `fmt-check` a soft, non-failing check
(the user chose enforcement); wiring CI now (deferred); putting the full `check`
gate in CMake while the Makefile is the documented bootstrap build.

## ADR-043 — `BY NAME` assignment: layout-independent member matching

**Context.** Rule (86) is `assignment-statement ::= {,• reference•••} = expression
[ , BY NAME ] ;`. Unlike whole-structure assignment (ADR-040), which copies only
between structures of *identical shape*, `BY NAME` assigns a structure from the
members of another structure that *share the same name*, regardless of layout.
The M2 scope names it as a distinct item.

**Decision.** `S = T, BY NAME` requires a **single** whole-structure target and a
whole-structure value (sema `structLeafType`); with multiple targets or a
non-structure operand it is diagnosed. Sema (`checkByNameMatch`) walks the target
structure's members, finds the same-named member of the source when present, and
checks the pair is assignable — recursing into minor structures, requiring
identical array types, and rejecting CHAR members (rule 11, consistent with the
rest of the compiler). Members absent from either side are skipped, not errors.
IRGen (`emitByNameCopy`) emits a per-member copy by name: a scalar leaf is loaded,
converted and stored; an array member is memcpy'd whole; a minor structure recurses.

**Consequences.** Two differently-declared structures sharing member names can be
copied field-wise, enabling record-style programmes without identical layouts. The
parse of `, BY NAME` sets `Stmt::byName` (and its HIR mirror); sema runs before the
multiple-assignment path so a multi-target `BY NAME` is caught early. `BY NAME` in
a multiple-assignment list stays rejected.

**Rejected.** Requiring identical shapes (that is already ADR-040 and defeats the
point of `BY NAME`); serving CHAR members (not yet supported anywhere, so it would
introduce a half-working case); storing a materialised member-pair plan from sema
to IRGen (the two structure types let codegen re-derive the same name walk cheaply,
keeping the change KISS).

## ADR-044 — `INITIAL` iteration factors on arrays: parse-time item tree, sema expansion

**Context.** Rule (26) is `initial-attribute ::= INITIAL ( { initial-call |
initial-itemlist } )`; the itemlist (rules (28)-(31)) holds constants, iteration
factors `(n)`, `*` repeat-last, and nested groups. M0 served only a single scalar
constant. M2 names "`INITIAL` iteration factors" for populating arrays.

**Decision.** The parser builds an `InitItem` tree (Value/Iter/Repeat/Group) from
the itemlist. Sema (`expandInitItems`) expands it into a flat sequence of folded
element values (`Symbol::initElems`), resolving iteration factors and `*` at
semantic time, and requires the expanded count to equal the array extent (a
mismatch is diagnosed, rule (26)). IRGen stores one constant per element: the
`emitInitials` AUTOMATIC path emits per-element stores; `emitGlobals` builds a
constant-array initializer for STATIC. `INITIAL CALL` (rule 27) and `INITIAL` on a
structure remain diagnosed. Character-element arrays stay diagnosed (rule 12).

**Consequences.** `DECLARE A(6) INIT((3) (1,2))`, `INIT((1,2,*,*))`, and nested
factors fill arrays element-by-element. The existing single-value scalar path is
unchanged (a one-item plain list still sets `item.init`). `*` repeats the already-
folded last value, so a repeated negative constant is not double-negated.

**Rejected.** Carrying the `InitItem` tree through HIR (sema already flattens to
`sym->initElems`, which codegen reads via the symbol); folding element values in
place twice (breaks `*` over a negative constant); padding a short list to the
extent (PL/I requires a full list; a mismatch is an error); implementing
`INITIAL CALL` now (independent, deferred).

## ADR-045 — `LIKE` template: the item deep-copies the referenced structure's type

**Context.** Rule (43) is `like-attribute ::= LIKE unsubscripted-reference`. The
declared item takes the structure shape of an already-declared structure
variable, so its members are qualified like the template's. M2 names "`LIKE`".
A template is only a shape; the copy and the template are distinct storage.

**Decision.** The parser records the template reference in `DeclItem::like`
(qualified `S.A.B` templates are diagnosed, rule (43)). Sema, when building an
item's type in `collectDecls`, deep-copies the template symbol's `Type` (the
same struct `Type` used by ordinary level-numbered structures) onto the item.
Because the copy is a fresh struct type, top-level `1 T LIKE S;` and nested
`2 M LIKE S;` (a member of S's shape) both work, and whole-structure assignment
`T = S;` works when the shapes match. A non-structure or undeclared template is
diagnosed; LIKE combined with a dimension (an array of the template) or with its
own member list (the extend form) is diagnosed, never silently dropped.

**Consequences.** `LIKE` needs no IRGen or HIR changes: the copied struct type is
handled by the existing structure code paths. `tests/core/like.pli` covers a
top-level copy, a nested member copy, and whole-structure assignment between
template and copy.

**Rejected.** Sharing the template's `Type` (member paths and layouts would alias
the template); emitting a member-tree copy into `children` (the struct `Type`
already carries the full shape); allowing a dimension after `LIKE` (an array of a
template needs the whole-template dimension machinery, independent); allowing the
extend form (members after `LIKE`); resolving qualified `S.A.B` templates now.

## ADR-046 — Cross-sections: a `*` axis marker and a gather loop on assignment

**Context.** Rule (126) is `unqualified-reference ::= identifier [ ( {,• {
expression | * }•••} ) ]`; a `*` subscript selects every index along that axis,
yielding a cross-section — a reduced-rank array value (`A(3,*)`, `X(1,*,3)`).
M2 names cross-sections for matrix programs; the core use is copying a row or
column out of a 2-D array into a 1-D array.

**Decision.** A `*` is parsed as a distinct `Star` expression node held in the
subscript argument list (an axis marker, not a value). Sema types a subscript
with at least one `*` as an array whose rank and bounds are those of the `*`
axes in order; the fixed axes are collapsed by their indices. Exactly one `*` is
served in this stage; more than one is diagnosed (rule (126)). The only use is
`B = A(i, *)` with `B` a whole array of the reduced shape (validated in sema).
IRGen emits a gather loop (`emitCrossSectionAssign`): the fixed-axis indices are
evaluated and SUBSCRIPTRANGE-checked once, then the `*` axis is iterated,
reading each source element by row-major flat offset and storing it into the
target's corresponding slot. A cross-section used anywhere else (as a general
expression value) is diagnosed.

**Consequences.** The row/column copy needs no new AST kind beyond the `Star`
marker and no HIR change beyond mirroring it; the existing `arrayElementAddr`
strides and `memberAddr` addressing are reused for the source and target bases.
`tests/core/cross_section.pli` covers a row, a column, and an array-member
cross-section; `bad_cross_section.pli` covers a scalar target, more than one
`*`, and a shape mismatch.

**Rejected.** Multi-`*` cross-sections now (a reduced-rank sub-block needs a
general nested-loop gather and broader value semantics); a cross-section as a
first-class expression value / function argument (array-value semantics,
independent); reusing `arrayElementAddr` per gather step by wrapping the
induction variable in an `IntLit` (fixed indices may be runtime values, so the
source flat offset is computed incrementally instead).

## ADR-047 — `DEFINED` overlays: no storage, an address alias to the base

**Context.** Rule (24) is `defined-attribute ::= DEFINED basic-reference
[ POSITION ( integer ) ]`. A `DEFINED` declaration overlays the storage of
another variable, so the two names touch the same bytes. ADR-018 framed this as
an address computation over the base, with alias metadata for the optimizer.

**Decision.** `DECLARE Y ty DEFINED X;` where `X` is an already-declared variable
of the same type in the same procedure makes `Y` an alias for `X`'s storage: sema
records `Y.sym->definedBase = X` and omits `Y` from the procedure's allocation
list; `IRGen::addressOf(Y)` returns `addressOf(X)`. Because `Y` and `X` share a
type, loads and stores through either name use the same LLVM pointer type, so the
overlay is a pure address alias with no copying and no reinterpretation. The base
is required to be same-type, in-scope, and not a structure; `POSITION`, a
subscripted base, a different-type (memory-view) overlay, and an `iSUB` subscript
(rule 134) are diagnosed rather than partially accepted.

**Consequences.** Writing through `Y` is immediately visible through `X` and vice
versa for both scalars and arrays. No new HIR node is needed — the alias lives on
the `Symbol`, so every existing load/store/address path works unchanged.
`tests/core/defined.pli` covers a scalar and an array overlay; `bad_defined.pli`
covers an undeclared base and a type mismatch.

**Rejected.** Copy-in/copy-out (wrong semantics — the whole point is shared
storage); emitting alias.scope/noalias metadata now (the optimizer has no
visible-alias bug at -O0, and both names already share one alloca, so LLVM sees
the aliasing structurally); a different-type overlay (needs an address bitcast and
a size check, an independent slice); `POSITION`, subscripted bases, and `iSUB`
(rule 134) now.

## ADR-048 — iSUB and subscripted `DEFINED`: an element-address redirect to the base

**Context.** Rule (24) `defined-attribute ::= DEFINED basic-reference
[POSITION(integer)]` and rule (134) `isub ::= integer SUB`. A `DEFINED` base may
be subscripted; an `iSUB` dummy variable (`1SUB`, `2SUB`) in that subscript
stands for the DEFINED array's own index, transforming it into a subscript of
the base (`DECLARE Y(5) DEFINED X(2*1SUB)`). ADR-018 framed these as address
computations over the base.

**Decision.** `integer SUB` is lexed as a distinct `Isub` token (no blanks, rule
134). A `DEFINED` base subscript list holds either a constant integer or a single
`iSUB` dummy. In sema a base with only constant subscripts makes the item a
scalar overlay of one element (stable GEP, `definedConstAddr`); a base with one
`iSUB` makes the item a 1-D array overlaying that axis of X. IRGen redirects an
iSUB-defined `Y(k)` (in both the load and store subscript paths) to the base
element `X(fixed..., k, fixed...)` via `definedSubElementAddr`, bounds-checking
the `k` slot; the fixed subscripts were compile-time checked. Writes through the
overlay are visible in the base and vice versa, because both name the same bytes.

**Consequences.** A row view `DECLARE R(4) DEFINED A(2, 1SUB)` is a live overlay
(unlike the copy produced by the `A(2, *)` cross-section, ADR-046). No copying
and no alias metadata needed: `Y(k)` and `X(2, k)` both resolve to the same GEP.
`tests/core/isub_defined.pli` covers a row overlay, a whole-array iSUB, and an
element overlay; `bad_isub_defined.pli` covers two iSUBs, a scalar item on an
iSUB base, a non-constant subscript, and an out-of-bounds constant.

**Rejected.** General iSUB index arithmetic (`X(2*1SUB)`, a transformed offset)
now (needs evaluating an index expression against the iSUB variable, an
independent slice); multi-axis iSUB (a sub-block overlay, broader address logic);
non-constant fixed subscripts (would need the address recomputed per reference
rather than at a stable point).

## ADR-049 — Multi-`*` cross-sections: a general affine gather over the star axes

**Context.** ADR-046 served exactly one `*` axis (`A(i, *)`, `A(*, j)`), a 1-D
row or column copy, and diagnosed more than one `*` as unimplemented. M2 names
cross-sections for matrix and table programs; copying a sub-block (`A(*, *)`,
`D(2, *, *)`, `D(*, 3, *)`) out of an N-D array is the same gather with several
star axes. Sema already reduced the type of any cross-section to the rank of its
`*` axes in order (`crossSectionType`), so only the one-star guard and the 1-D
gather loop were the limiting pieces.

**Decision.** The `nStar > 1` diagnostics are removed: sema types any
cross-section to the array of its `*`-axis dims in order, and the existing
whole-array-target shape check validates it. `emitCrossSectionAssign` becomes a
general affine gather. The fixed axes' indices are evaluated and
SUBSCRIPTRANGE-checked once and folded into a fixed source flat offset; then the
target's linear row-major index is iterated, decomposed into star-axis
coordinates against the target strides, and each coordinate maps back to a
source flat offset through the corresponding star axis's stride. A single `*` is
the rank-1 case of the same loop. Bounds are identical on both sides (the target
is the reduced array), so coordinates run `0..extent-1` and all offset
arithmetic is unsigned.

**Consequences.** Sub-block copies need no new AST/HIR node beyond the existing
`Star` marker. `tests/core/cross_section2.pli` covers `A(*, *)`, `D(2, *, *)`,
`D(*, 3, *)`, and a two-star array-member cross-section `S.M(2, *, *)`;
`bad_cross_section.pli` still rejects a cross-section to a scalar or a
shape-mismatched target (including a multi-`*` cross-section of the wrong
shape). GRAMMAR-COVERAGE rule (126) now lists multi-`*` copies as served.

**Rejected.** A cross-section as a general expression value or function argument
(still an array-value-semantics concern, independent of the gather); dynamic
(non-constant) cross-section bounds via a runtime dope vector (deferred with
dynamic extents generally); nesting the gather as one loop per `*` axis rather
than a single linear decomposition (more basic blocks for no benefit at this
stage).

---

## ADR-050 — Dynamic array extents: a single-axis `AUTOMATIC` array with a runtime upper bound

**Context.** M2 names dynamic bounds (rule (13)) for matrix and table programs.
The constant-bounds path (`ADR`-adjacent to (12)) lays out `[N x elemTy]` arrays
in the frame, so an extent must be a compile-time constant. A runtime extent
`DECLARE A(n) ...` cannot be an LLVM aggregate field; it needs a buffer sized at
block entry from the live value of `n`, plus a way to bounds-check subscripts
against that runtime bound.

**Decision.** This stage serves exactly one dynamic form: a single-axis
`AUTOMATIC` array with a constant lower bound (default 1) and a runtime upper
bound, e.g. `A(n)` or `A(lb:n)`. The upper-bound expression is kept on the
`DeclItem` (`dynBounds`), lowered to HIR and stashed on `Symbol::dynUb`, and run
through sema's `typeExpr` so its symbol resolves. `allocaLocals` evaluates it in
a second pass (after fixed-size locals, so referenced variables are addressable),
allocates `alloca i32, i64 extent` where `extent = ub - lb + 1`, records the
buffer in `symAddr_` and the live bound in a `dynUb_` dope slot. `arrayElementAddr`
takes the dynamic 1-D path: one runtime SUBSCRIPTRANGE against the recorded bound,
then `GEP i - lb` on the bare element pointer. `LBOUND` reports the constant lower
bound; `HBOUND`/`DIM` read the recorded runtime bound. Because AUTOMATIC storage
is sized at entry, the bound is evaluated at block entry and fixed for the block's
lifetime.

**Consequences.** `tests/core/dynamic_array.pli` drives the bound via a procedure
parameter and checks runtime read/write and `LBOUND`/`HBOUND`; `bad_dynamic_array.pli`
and `bad_dynamic_array_ext.pli` reject the unsupported forms. Dynamic arrays cannot
be structure members, multi-axis, or carry `INITIAL` in this stage (diagnosed, rule
(13)/(26)); `*` adjustable extents and dynamic lower bounds are rejected at parse
time. GRAMMAR-COVERAGE rules (12),(13) and the implementation table list the served
form.

**Rejected.** Dynamic structure members and multi-axis dynamic arrays (need dope
vectors; ADR-008 defers them); `*` adjustable extents (a descriptor with
re-allocation on entry); a dynamic lower bound (bounds stay constant here); a
heap allocation for dynamic arrays (the runtime-sized `alloca` is valid for the
block-scoped AUTOMATIC lifetime and needs no deallocator); evaluating the bound
lazily at each subscript instead of once at entry (would allow the extent to
change mid-block, which AUTOMATIC semantics forbid).

---

## ADR-051 — Arrays of structures: a dimensioned level item reached by subscript-then-qualify

**Context.** M2 names nested arrays of structures. A structure whose level item
carries a dimension, `1 arr(3), 2 x, 2 y`, is an array whose elements are
structures; a member of one element is `arr(i).x` — subscript the array to a
structure element, then qualify into a member. The existing access forms were
qualify-then-subscript (`S.A(i)`, a structure with an array *member*, ADR-037) and
plain array subscript (`A(i)`); `arr(i).x` (subscript *then* qualify) was
unparseable, and `buildType` dropped a level item's dimension when it had members,
so `arr` was typed as a plain structure rather than an array of structures.

**Decision.** An array of structures is represented as a structure `Type` that
keeps its `dims` (`buildType` copies `it.ty.dims` onto the built struct type), so
`isArray()` and `elementType().isStruct()` are both true and `llvmTy` lays it out
as `[N x structTy]`. The parser collects member qualifiers after the subscript
group (`parsePrimary`), so `arr(i).x` is a `VarRef` with `args=[i]`, `path=[x]`;
sema's `Call` case recognizes a base that is an array of structures, checks the
subscript count against the array dims, reclassifies as `Subscript`, and resolves
the path against the element structure type. Codegen addresses a member of one
element as `arrayElementAddr` to the element struct, then `elementMemberAddr` — a
GEP through the recorded field indices from the caller-supplied element address,
mirroring `memberAddr` which starts from the symbol's own base. The parse-time
dimension heuristic is extended: a bare extent `(n)` directly after a name is a
dimension when an attribute keyword *or a comma* follows it, because a scalar
precision can never follow a name bare, and the comma is the structure-array form
`1 arr(3), 2 x`.

**Consequences.** `tests/core/struct_array_of.pli` writes and reads `arr(i).x`/
`arr(i).y` with runtime subscripts and in expressions; `bad_struct_array_of.pli`
rejects a wrong subscript count and a missing member. A member array of an element
(`arr(i).x(j)`) is diagnosed unimplemented. GRAMMAR-COVERAGE rules (11),(124),(126)
and the implementation table list the served form.

**Rejected.** A member array of an element (`arr(i).x(j)`) — two-level subscripting
in one reference, deferred until the array-of-structures access is well covered; a
whole element as a value (`arr(i)`) — already diagnosed as a whole-structure value
(rule 127); a distinct AST/HIR node for subscript-then-qualify — the existing
`VarRef` with `args`+`path` and the `Subscript` reclassification carry it without
new nodes.

---

## ADR-052 — INITIAL CALL: a function-call initializer evaluated at block entry

**Context.** M2 names `INITIAL CALL` (rule 27). The existing `INITIAL` machinery
folds the value to a compile-time constant (`foldInitialConstant` → `initExpr`,
or an array element list `initElems`), which cannot represent a runtime call.
`INITIAL(CALL f(args))` was diagnosed at parse time.

**Decision.** `INITIAL(CALL identifier ( argumentlist ))` (rule 27) is parsed as a
function-call expression and carried on the `DeclItem` (`initCall`), distinct from
the scalar-constant `init`. Sema type-checks it with `typeExpr`, which resolves the
function, its arguments, and its return type, and rejects a non-value-returning
(plain) procedure; the call is stashed on the symbol. HIR lowering owns the lowered
call on `HDeclItem.initCall` and rides a raw `HExpr*` (`sym->initCallH`) on the
symbol (mirroring the `dynUb` pattern, ADR-050). `emitInitials` evaluates the call
via `emitExpr` at block entry and stores the return value through `storeTo`, which
converts it to the variable's type. Because STATIC storage is not implemented
(storage classes are accepted-but-inert), the call runs on every procedure entry,
matching AUTOMATIC semantics.

**Consequences.** `tests/core/init_call.pli` initializes two variables from a
value-returning function with different arguments; `bad_init_call.pli` rejects
calling a procedure that does not return a value. `INITIAL CALL` in a factored
declaration is diagnosed. GRAMMAR-COVERAGE rules (26)-(32) and the implementation
table list the served form.

**Rejected.** `INITIAL CALL` with no argument list (`CALL f` without parens) — rule
(27) requires `( argumentlist )`; a call initializer for an array or structure
initializer list — served only for a scalar variable here, arrays/structures
continue to use the constant itemlist; a one-time (STATIC) evaluation — STATIC is
not yet implemented, so the call runs each entry, and a genuine STATIC `INITIAL CALL`
is deferred with STATIC storage.

## ADR-053 — iSUB index arithmetic: an affine 1-D overlay of one base axis

**Context.** ADR-048 served a bare `iSUB` dummy (`X(1SUB)`) making `Y(n)` a 1-D
overlay of one axis of `X`, and explicitly rejected the general form `X(2*1SUB)`
(a transformed offset). Rule (134) `isub ::= integer SUB` permits the iSUB dummy
to appear within the arithmetic of a base subscript. This ADR covers the affine
form.

**Decision.** A `DEFINED` base subscript that is an iSUB dummy may carry an
affine coefficient and offset, giving the base index `m*1SUB + c` for the axis
(`X(2*1SUB)`, `X(1SUB+2)`, `X(2*1SUB-1)`). In the AST each `DefinedSub` carries
`mult` (m, default 1) and `add` (c, default 0); a bare `1SUB` is `m=1, c=0` and a
constant base subscript stays a fixed `expr`. Parser accepts `[m] [*] 1SUB [+/- c]`
and rejects a non-affine iSUB (a second linear occurrence such as `1SUB*1SUB` is
rejected at parse, per rule (126)). Sema checks the overlay's affine image: with
`Y` ranging `[lb,ub]`, the image `m*[lb,ub]+c` must stay within the base axis
bounds `[lo,hi]`, swapping the endpoints when `m<0`, and records `definedIsubMult`
and `definedIsubAdd` on the symbol. IRGen computes the base index `m*yidx+c` for
both the bounds check and the flat element address.

**Consequences.** `tests/core/isub_arith.pli` covers a strided (`2*1SUB`), an
offset (`1SUB+2`), and a combined (`2*1SUB-1`) overlay, each checked both by
reading through the overlay and by writing through it and reading the base;
`bad_isub_arith.pli` rejects an out-of-range image (`2*1SUB` on a 6-element
overlay of a 10-element base) and a non-linear iSUB. As in ADR-048, the overlay
is a live alias to the base bytes, so writes through it are visible in the base
and vice versa.

**Rejected.** Multi-axis iSUB (a sub-block overlay with an iSUB on more than one
axis — the affine image is computed per axis, but the address logic for a
multi-axis sub-block remains broader); a non-affine iSUB (e.g. `1SUB*1SUB`); a
non-constant coefficient or offset (constant-folded only in this stage).

## ADR-054 — Dynamic array parameters: a by-reference bound argument sized at callee entry

**Context.** ADR-050 served single-axis dynamic (`AUTOMATIC`) arrays **locally**:
`allocaLocals` evaluated the runtime upper bound at entry and recorded it in a
`dynUb_` dope slot, so subscripting and the array built-ins bounds-check against
the live extent. But a dynamic array passed to a procedure (`DECLARE X(K) ...` as a
parameter, rule (34)) has no local alloca and no entry in `dynUb_` — every
parameter is a by-reference pointer to the caller's data. Subscripting such a
parameter crashed irgen because `arrayElementAddr` received a null dynamic bound,
and the SUM/PROD/ANY/ALL reduce loop assumed a fixed `[N x elem]` layout.

**Decision.** Record a dynamic array **parameter's** runtime upper bound at entry,
mirroring ADR-050's locals. A new `recordDynParamUbs` runs after `allocaLocals`
(in both the plain and multi-entry procedure paths) and, for each dynamic-array
parameter, evaluates `Symbol::dynUb` (the bound argument, itself a by-reference
parameter, e.g. `k` in `x(k)`) and records it in the same `dynUb_` dope slot. The
parameter keeps `symAddr_` as the caller's data pointer, so `arrayElementAddr`'s
dynamic 1-D path then bounds-checks and GEPs the bare element pointer against the
live bound exactly as for a local. The SUM/PROD/ANY/ALL reduce loop is generalized
to the dynamic case: it drives the element count from the recorded bound (a runtime
value) and addresses a bare element pointer instead of the fixed `[N x elem]` GEP.

**Consequences.** `tests/core/dyn_param.pli` passes a fixed array to a procedure
whose parameter is dynamic, verifying by-reference writes (visible in the caller),
`LBOUND`/`HBOUND`/`DIM`/`SUM` against the live extent, and a value-returning
function with a dynamic array parameter; `bad_dyn_param.pli` rejects a multi-axis
dynamic parameter and a dynamic lower bound (rule (13)). The bound is read once at
entry, so it cannot change mid-block (matching AUTOMATIC semantics, ADR-050).
GRAMMAR-COVERAGE rules (12),(13) and (34)-(38) list the served form.

**Rejected.** Multi-axis dynamic parameters and a dynamic lower bound on a
parameter (need dope-vector descriptors, ADR-008); `*` adjustable-extent
parameters (a descriptor with re-allocation on entry); passing a dynamic array
that is a structure member (dynamic structure members remain diagnosed).

## ADR-055 — `*` adjustable-extent array parameters: a hidden extent argument

**Context.** ADR-054 served dynamic array parameters whose extent comes from an
explicit bound argument (`x(k)` bound by `k`). PL/I table programs also use the
`*` adjustable extent: `DECLARE X(*)` as a parameter takes its extent from the
caller's actual array, with no named bound argument. Rule (13) marks the axis
`dyn`; previously the parser diagnosed `*` outright.

**Decision.** `*` is accepted in a declaration dimension and recorded as a
distinct `Dim::adj` (adjustable) flag alongside `dyn`, so it is told apart from a
dynamic-bound axis (which has a bound expression). Sema gates it: a `*` axis is
valid only on a parameter, and only single-axis in this stage; a block-scope `*`
is diagnosed (rule 13). The extent flows caller-to-callee as a hidden i64
argument per `*` parameter, placed after the by-reference pointers and before the
static links. `declareProc` (plain impl, multi-entry impl, and each entry thunk)
and `calleeFn` add the i64 extent to the signature; `emitCall` and the
expression-call path compute the caller's actual element count (`argExtent`: a
constant for a fixed array, the live bound for a dynamic-bound array) and pass
it; the callee entry reads it into the same `dynUb_` dope slot as ADR-050/054, so
subscripting bounds-checks and the LBOUND/HBOUND/DIM/SUM/PROD built-ins report
against the live extent.

**Consequences.** `tests/core/star_param.pli` passes a fixed, a dynamic-bound, and
a multi-axis (first-axis extent) array to `*` parameters, verifies by-reference
writes are visible in the caller, and checks LBOUND/HBOUND/DIM/SUM in the callee;
`bad_star_param.pli` rejects a block-scope `*` and a multi-axis `*` parameter.
`bad_dynamic_array_ext.pli` still rejects a block-scope `*`. The hidden extent
argument is consistent across plain and multi-entry procedures and their thunks.
GRAMMAR-COVERAGE rules (12),(13) and (34)-(38) list the served form.

**Rejected.** Multi-axis `*` parameters (need dope-vector descriptors, ADR-008);
forwarding one `*` array to another `*` parameter (a second hidden extent whose
value is not a constant or a recorded dynamic bound); a dynamic lower bound on a
`*` parameter; `*` on a non-parameter declaration (a `*` local has no caller to
supply its extent).

---

## ADR-056 — Exact FIXED DECIMAL constants and scale-aware conversion

**Context.** ADR-006 represents `FIXED DECIMAL(p,q)` as an integer scaled by
10^q but left `scale` unused in codegen (M0 deviation: scale 0 only). QR1.2
needs exact decimal: a bare fractional literal (`2.5`) is a FIXED DECIMAL
constant (rule 135), and arithmetic, comparison, and assignment must honour
the scale.

**Decision.** A numeric literal with a fraction point and no exponent becomes an
exact `FIXED DECIMAL(p,q)` constant (`p` = digits, `q` = fraction digits, stored
integer = value·10^q); an exponent form stays FLOAT. `IRGen::convert` rescales a
FIXED DECIMAL value by powers of ten: to a DECIMAL target by 10^(dst−src), and a
scaled DECIMAL source to a BINARY target drops the fraction; scale reduction
rounds half away from zero. `FLOAT ↔ FIXED DECIMAL` scales by 10^q. `+ -` and
comparison rescale operands to the common (max) scale; `*` (and `MULTIPLY`)
multiply the raw scaled integers (product scale = sum) without pre-rescaling.
`arithResultType` yields a DECIMAL common type only when both operands are
DECIMAL, so FIXED BINARY scale (2^q) is never rescaled by ten.

**Consequences.** `tests/core/decimal.pli` covers entry from a FLOAT literal,
`FIXED DECIMAL ↔ FLOAT`, scale-aware `+ - *`, mixed-scale addition, product-scale
widening, round-half-away scale reduction, and scaled comparison. The existing
`scaled.pli` (FIXED BINARY, 2-based) and the float-literal builtin tests
(TRUNC/ROUND/MAX/MOD/PRECISION/MULTIPLY/ABS) stay green. `TRUNC` of a scaled
DECIMAL now drops the fractional digits. FIXED division still evaluates in FLOAT
(ADR-006 deviation, QR2).

**Rejected.** Binary floating point for decimal (0.10 not representable);
changing `scaled.pli`'s 2-based FIXED BINARY behavior; adding decimal overflow
(`SIZE`/`FIXEDOVERFLOW`) checks in this slice (deferred to QR2).

## ADR-057 — `INITIAL` on a structure: a per-leaf store walk

**Context.** ADR-044 expands an `INITIAL` itemlist for a fixed-size array into a
flat list of folded element values stored element-by-element (AUTOMATIC) or as a
constant array initializer (STATIC). A structure (rule 11) `INITIAL` was
diagnosed. CM1 needs structure initialization: a flat itemlist must fill the
structure's scalar leaves in declaration order.

**Decision.** The itemlist (with iteration factors, `*`, and groups) is first
flattened into raw values, then folded against each leaf's own type in
declaration order (`structureLeafCount` checks the count; `foldStructInit` walks
members — a nested structure recurses, an array member consumes its element
count, an array-of-structures recurses per element). The folded list reuses
`sym->initElems`. `emitInitials` stores it with a recursive member walk
(`emitStructInitValues`) mirroring `emitByNameCopy`'s GEP pattern, so it runs on
every activation for AUTOMATIC storage. STATIC structure `INITIAL` stays
diagnosed; CHARACTER leaves stay diagnosed (matching the existing "CHARACTER
structure members are not implemented" limitation).

**Consequences.** `tests/core/struct_init.pli` covers scalar, nested, and array
members, iteration factors, `*`, and re-initialization on each activation;
`bad_struct_init.pli` diagnoses a count mismatch. `make test` (123) and `make
check` stay green.

**Rejected.** Folding during itemlist expansion (the leaves are heterogeneous, so
a value must be folded against its own member type, not a single element type);
STATIC constant structure initializers and CHARACTER members in this slice
(deferred).

## ADR-058 — Dynamic lower bound: a runtime lower bound on a 1-D AUTOMATIC array

**Context.** ADR-050 serves a single-axis AUTOMATIC array with a runtime *upper*
bound (`dyn`/`dynUb`), a constant lower bound. A non-constant lower bound was
diagnosed at parse time. CM1 needs `A(lb:ub)` with both bounds runtime.

**Decision.** `Dim` gains `lbDyn` (marking a runtime lower bound; `isDynamic()`
includes it); the parser captures the lower-bound expression into a parallel
`dynLbBounds` (mirroring `dynBounds`), lowered to `Symbol::dynLb`, evaluated at
block entry into a `dynLb_` dope slot in `allocaLocals`. `arrayElementAddr` (and
its callers) bounds-check `i < lb` and offset `i - lb` against the live value;
`LBOUND`/`DIM`/reduction extents and `argExtent` use it. Scope stays single-axis
AUTOMATIC: a dynamic lower bound on a *parameter* is diagnosed (the dyn-param /
`*` calling conventions convey only the upper bound/extent, so a lower-bound
parameter would be sized from the wrong origin).

**Consequences.** `tests/core/dyn_lower.pli` covers runtime `LBOUND`/`HBOUND`/
`DIM`, fill/readback, and a `SUM` reduction over `A(lb:ub)` for several bound
pairs; `bad_dyn_lower.pli` keeps the multi-axis gate. `make test` (125) and
`make check` stay green.

**Rejected.** Threading a lower-bound argument through the dyn-param / `*`
calling conventions (extend the ABI in a later slice); multi-axis dynamic
arrays; a dynamic lower bound on a parameter in this slice.

## ADR-059 — Dynamic multi-axis arrays: a runtime first axis with fixed later axes

**Context.** ADR-050/058 serve a single-axis AUTOMATIC array with a runtime upper
and/or lower bound, allocated as a bare element buffer (offset `i - lb`). A
multi-axis array with any dynamic axis was gated ("must be single-axis"). CM1
needs `A(n,4)` — a 2-D VLA (C analogue `int a[n][4]`) whose first axis is runtime
and later axes are fixed.

**Decision.** Only the **first** axis may be dynamic; later axes stay constant.
The storage is a bare element buffer of `axis0_extent × ∏(later extents)`.
`arrayElementAddr`'s dynamic path accumulates a row-major flat offset with a
compile-time first-axis stride (the product of the fixed later extents) and
per-axis bounds checks (axis 0 against the runtime bounds, later axes against
their constants). `allocaLocals`, `DIM`, reduction counts, and `argExtent`
scale by the fixed later extents; LBOUND/HBOUND report the (runtime) first axis.
Sema rejects a dynamic axis beyond the first and a dynamic lower bound on a
parameter (the calling conventions convey only the upper bound/extent).

**Consequences.** `tests/core/dyn_multi.pli` covers `A(n,4)` for several `n`
(runtime LBOUND/HBOUND/DIM, row-major fill/readback); `bad_dyn_multi.pli`
rejects `A(3,n)`; a constant-upper dynamic array `A(lb:5,3)` runs (the 
`allocaLocals`/LBOUND paths no longer assume a runtime upper bound). `make test`
(127) and `make check` stay green.

**Rejected.** A dynamic extent on any axis beyond the first in this slice (needs
runtime strides for each such axis, deferred); a dynamic multi-axis array passed
to a `*` parameter beyond the total-extent case; a dynamic structure member
(still gated).

## ADR-060 — Dynamic array structure members: a bare buffer-pointer field

**Context.** ADR-050/058/059 serve standalone AUTOMATIC arrays with a runtime
extent, allocated as a bare element buffer whose bounds are recorded at entry.
A dynamic array that is a structure member was gated ("cannot be a structure
member in this stage"); `struct_dyn.pli` needs `1 s, 2 v(n) fixed bin(31)`,
reached by qualification `s.v(i)` on both sides of an assignment.

**Decision.** A dynamic-array member lays out in the LLVM struct as a **bare
buffer pointer** field (`ptr`), not an inline `[N x elemTy]` — the extent is
runtime, so the struct size cannot be compile-time. `llvmTy` emits `ptr` for a
member whose type `isArray && isDynamic`. At block entry, `allocaLocals` pass 3
allocates each member's runtime-sized element buffer (bounds evaluated from its
bound exprs, scaled by any fixed later axes), stores the pointer into the struct
field, and records the live bounds in `memberDyn_` keyed by (symbol, field path).
Subscript addressing loads the buffer pointer (`dynamicMemberBase`) before the
`arrayElementAddr` dynamic path, bounds-checking against the recorded bounds.
The dynamic member bound exprs are lowered in `hir.cpp` and owned by
`HDeclItem::dynMemberBounds`, referenced (non-owning) by
`Symbol::DynMemberH.ub/lb`.

**Consequences.** `tests/core/struct_dyn.pli` covers a fixed scalar member beside
a dynamic member, element write/readback via `s.v(i)` in loops, and scalar-member
integrity across the dynamic buffer; `make test` stays green. Because a struct
field is now a pointer to separately-allocated data, a whole-structure storage
copy would copy the pointer, not the pointed-to data — `Sema::checkAssignable`
rejects a whole-structure assignment with a dynamic member (rule 13),
`bad_struct_dyn.pli`.

**Rejected.** An inline `[N x elemTy]` member for a dynamic array (struct size is
not compile-time); supporting whole-structure copy of a struct with a dynamic
member (needs deep copy of each buffer, deferred); a dynamic member of an array
of structures `arr(i).v(n)` (member buffer per element not yet served); `LIKE`
of a struct with a dynamic member is diagnosed, not deep-copied.

## ADR-061 — `INITIAL` on a dynamic array: fill the runtime buffer at entry

**Context.** ADR-044 serves an `INITIAL` itemlist on a **fixed-size** array by
expanding it (sema) into `sym->initElems` and storing one constant per element;
a count that does not match the extent is diagnosed at compile time. ADR-050+
serve dynamic (runtime-extent) AUTOMATIC arrays as a bare runtime-sized alloca,
but `INITIAL` on a dynamic array was still gated as rule (26) — the itemlist
cannot be count-checked because the extent is runtime.

**Decision.** `INITIAL` on a dynamic AUTOMATIC array is served by expanding the
itemlist into `sym->initElems` exactly as for a fixed array, but with **no
compile-time count check** (the extent is runtime). At block entry, `emitInitials`
GEPs each constant into its slot of the runtime-sized element buffer, straight
line — no loop — so it re-runs on every activation (AUTOMATIC semantics). To keep
a longer-than-extent itemlist from overflowing the buffer, `allocaLocals` pass 2
pre-sizes the alloca to the larger of the runtime extent and the itemlist length;
the logical extent used for bounds checks (`dynUb_` / `SUBSCRIPTRANGE` / `DIM`)
is unchanged. A dynamic lower bound and a dynamic first axis stay served by the
existing paths (ADR-058/059); multi-axis and structure-member dynamic arrays fill
the same flat row-major buffer.

**Consequences.** `tests/core/dyn_init.pli` covers a full matching itemlist with
an iteration factor, a list shorter than the extent (only the supplied elements
set; the rest stay uninitialized per AUTOMATIC semantics), a dynamic multi-axis
array filling flat row-major, and re-run on every activation; `make test` stays
green. `bad_dynamic_array.pli` no longer rejects `INITIAL` on a dynamic array and
now rejects only the multi-axis form.

**Rejected.** Compile-time count-checking of a dynamic itemlist (the extent is
runtime); zero-filling the rest of a short itemlist (AUTOMATIC leaves it
uninitialized, matching scalar/array semantics); a loop for the store (the
itemlist is a fixed constant sequence, straight-line GEPs are leaner).

## ADR-062 — Runtime aggregate lengths on a dynamic structure member

**Context.** ADR-060 lays out a dynamic-array structure member `1 s, 2 v(n) fixed
bin(31)` as a bare buffer-pointer field, with the live bounds recorded in
`memberDyn_` at entry. ADR-050/058/059 serve `LBOUND`/`HBOUND`/`DIM` and the array
reductions `SUM`/`PROD`/`ANY`/`ALL` for **standalone** dynamic arrays, reading the
bounds from `dynUb_`/`dynLb_` keyed by the array symbol. The same built-ins on a
**qualified** member array `S.V` were gated (rule 123): sema only recognized a
plain array `VarRef` (`a->sym->ty.isArray()`), and irgen read `a->sym->ty` and
`dynUb_[a->sym]`, which for `S.V` is the struct symbol, not the member.

**Decision.** The array-attribute and array-reduction built-ins accept an
unsubscripted **member array** reference `S.V` as their argument. Sema
(`Sema::typeBuiltin`) recognizes an unsubscripted array by the resolved reference
type `a->ty.isArray()` (a qualified `VarRef` resolves `a->ty` to the member array
type), instead of `a->sym->ty.isArray()`, and derives the reduction element type
from `a->ty.elementType()`. IRGen distinguishes a member array by
`a->memberPath` non-empty: the array type comes from `memberType(a->sym, path)`,
the live lower/upper bounds (incl. a dynamic lower bound) from the `memberDyn_`
slot, and the reduction base from `dynamicMemberBase(...)` (a dynamic member) or
`memberAddr(...)` (a fixed member) instead of `addressOf(a->sym)`. The reduction
loop then walks the member buffer pointer the same way as a standalone dynamic
array.

**Consequences.** `tests/core/struct_dyn_len.pli` covers `LBOUND`/`HBOUND`/`DIM`
and `SUM` on `s.v(n)` for several extents (including 0), a dynamic lower bound
member `s.v(2:n+1)`, and fills then reduces the member buffer;
`tests/core/bad_struct_dyn_len.pli` keeps a scalar member `s.a` rejected as an
array built-in argument (rule 123); `make test` stays green. A subscripted member
`S.V(i)` remains rejected (it is a `Subscript`, not an unsubscripted `VarRef`).

**Rejected.** Adding member knowledge to the symbol table (member bounds already
live in `memberDyn_`); requiring the whole structure as the built-in argument
(the built-ins reduce a single array, not a whole structure).

## ADR-063 — POINTER as a first-class address type

**Context.** QR1.3 (CM2 of the C-mirror sub-plan) needs `POINTER`, based data,
`->`, `ADDR`, `NULL`, and `ALLOCATE`/`FREE` for linked records. Before based
addressing and allocation can be built, POINTER must exist as a real value type:
declared `DECLARE P POINTER;`, holding the null pointer, the address of a
variable, or another pointer, assignable between pointer variables and compared
for equality/inequality.

**Decision.** Add a `TK::Pointer` scalar type (`Type::ptr()`, `isPointer()`). A
pointer variable is declared with the `POINTER`/`PTR` attribute (parsed into the
attribute bag and rejected if combined with a numeric/string attribute, rule 15)
and lays out in IR as an LLVM `ptr`. Pointer assignment copies the address
(`Sema::checkAssignable` allows pointer→pointer; `IRGen::convert` passes a
pointer through unchanged). `NULL` and `ADDR(x)` are built-ins typed in
`Sema::typeBuiltin` and emitted in `IRGen::emitBuiltin`: `NULL` yields a null
pointer constant, `ADDR(x)` the `addressOf` a variable. Pointer equality and
inequality are emitted as an integer `icmp eq/ne` on the two addresses; ordered
comparisons and mixing a pointer with an arithmetic value are diagnosed (rules
(15),(117)). Following the codebase's no-arg-builtin convention (DATE/TIME),
`NULL` is written `null()` with parentheses, not bare.

**Consequences.** `tests/core/pointer.pli` covers `p = null()`, `p = addr(x)`,
`q = p` pointer assignment, and `=`/`^=` comparisons against the null pointer and
other pointers; `tests/core/bad_pointer.pli` rejects assigning a pointer to a
numeric target; `make test` stays green. A pointer value cannot be written with
`PUT LIST` in this stage (diagnosed, rule 110); pointer parameters, based data,
`->`, and `ALLOCATE`/`FREE` remain for later CM2 slices.

**Rejected.** An integer-typed pointer (LLVM `ptr` keeps the address opaque and
avoids accidental arithmetic); allowing bare `NULL` without parentheses (kept
consistent with DATE/TIME and other no-arg built-ins in this compiler); pointer
arithmetic or ordering.

## ADR-064 — BASED data and `->` locator qualification

**Context.** ADR-063 added POINTER as a first-class address type. QR1.3 (CM2)
needs based data for linked records: `DECLARE 1 X BASED(P);` where X has no
storage of its own and its members are addressed through the POINTER P, plus the
explicit locator-qualified form `P -> X.FIELD` (rule 124). Both were diagnosed as
unimplemented.

**Decision.** A based structure is a Symbol with a `basedBase` POINTER reference
(resolved in sema from the `BASED(P)` attribute; `BASED` without an explicit
pointer is diagnosed in this stage). It is excluded from `localSyms` (like
`DEFINED`), so it gets no own storage. `IRGen::addressOf(basedSym)` loads the
pointer value (`load(addressOf(basedBase))`), so an unqualified based reference
`X.FIELD` naturally GEPs off the pointer. A locator-qualified reference is a
`VarRef` carrying the left-hand pointer in a new `locPtr` field (mirrored through
HIR lowering); sema types it (locator must be a POINTER, right side a based
variable) and resolves the member path against the based structure type, and irgen
GEPs off the loaded pointer value (`locatorMemberAddr`) in both value emission and
assignment targets.

**Consequences.** `tests/core/based.pli` covers pointing P at an existing
structure via `addr(y)`, writing/reading `rec.a` through P, and the locator form
`P -> rec.a` on both sides of an assignment, observing the writes in `y`'s
storage; `tests/core/bad_based.pli` rejects a locator whose right side is not a
based variable; `make test` stays green. A whole based structure as a value, based
array subscripts (`P -> X.arr(i)`), and `ALLOCATE`/`FREE` remain for later CM2
slices.

**Rejected.** Giving a based structure its own storage (it overlays the pointer's
target); requiring the locator pointer to equal the based structure's own
`BASED(P)` pointer (the locator may name any pointer); supporting bare `BASED`
without a pointer (needs an unqualified-locator rule deferred with
`ALLOCATE`/`FREE`).

## ADR-065 — ALLOCATE/FREE for based records via heap malloc/free

**Context.** ADR-063/064 added POINTER and based data with `->`. QR1.3 (CM2)
needs the last piece of linked records: `ALLOCATE` (rules 87-88) to create the
heap storage a based structure overlays and `FREE` (rule 90) to release it. Both
were diagnosed unimplemented, citing rule (87).

**Decision.** `ALLOCATE id SET(ref);` heap-allocates the based structure `id`
and stores the address in the POINTER `ref`; `FREE P -> id;` releases the block
addressed by the locator P, and `FREE id;` releases the block addressed by the
based variable's own `BASED` base. Both map to C `malloc`/`free` via two new
runtime entries `pli_alloc`/`pli_free` in `pli_rt_abi.def`. `emitAllocate` sizes
the block from the based structure's LLVM alloc size
(`getTypeAllocSize(llvmTy(sym->ty))`) and stores the returned pointer into the
SET target (a normal pointer `storeTo`); `emitFree` calls `pli_free` on the
loaded locator value or on `addressOf(basedSym)`. Sema requires a based variable
(a `basedBase` symbol) and a POINTER SET target/locator (rule 88/90), and defers
dynamic-extent based arrays and the `IN (AREA)` option to QR2.3.

**Consequences.** `tests/core/alloc.pli` covers two allocations of one based
variable producing independent blocks, locator read/write, and both FREE forms;
`tests/core/bad_alloc.pli` rejects allocating a non-based variable and
`tests/core/bad_alloc_set.pli` a non-pointer SET target; `make test` and
`make check` stay green. `pli_alloc` raises a hard ALLOCATION error on OOM until
condition handling (M4).

**Rejected.** Calling libc `malloc`/`free` directly in emitted IR (kept behind
the `pli_rt_abi.def` ABI so the C and IR signatures cannot drift, cf. ADR-002);
reusing the based structure's own `BASED` pointer for `ALLOCATE SET` (SET may
name any pointer); serving the `IN (AREA)` option, which needs a runtime
sub-allocator.

## ADR-066 — GET LIST list-directed input from SYSIN

**Context.** CM3 (QR1.5) of the C-mirror sub-plan needs list-directed input, the
input counterpart to the existing `PUT LIST`. The `GET` statement (rules
104-109) was diagnosed unimplemented citing rule (104).

**Decision.** `GET [SKIP] LIST (datalist);` reads list-directed values from SYSIN
(stdin) into the data-list references, which sema requires to be assignable
scalar variables (an array element, a structure member, or a plain variable),
not constants or procedures (rule 110). `GET` mirrors `PUT`: it is a
`Stmt::Get`/`HStmt::Get` statement reusing the `items` data list. Four new
runtime entries (`pli_get_list_fixed/float/char/bit` in `pli_rt_abi.def`) read a
whitespace/comma-delimited token from stdin and parse it as i64, double, a
blank-padded character field, or a bit. `emitGet` produces a value of the item's
type and stores it with the same target-addressing as an assignment
(`storeGetTarget`); scaled FIXED DECIMAL input is read as a plain integer and
converted, and `GET` of a CHARACTER member or array element is diagnosed (the
member/array-element character store paths are not served, matching assignment).

**Consequences.** `tests/driver/get.sh` compiles a program that `GET LIST`s a
pair of FIXED, a FLOAT, and a CHARACTER value, verifies each, and prints PASS;
`tests/core/bad_get.pli` rejects reading into a constant; `make test` and
`make check` stay green.

**Rejected.** Using C `scanf` directly in emitted IR (kept behind the
`pli_*` ABI so signatures cannot drift, cf. ADR-002); supporting `FILE`/`STRING`
sources, `EDIT`/`DATA` specifications, `COPY`, `LINE`, or `PAGE` in this stage
(QR2.5); list-directed `DO`-repetition data-list elements (rule 111).

## ADR-067 — STRING ( reference ) list-directed sinks and sources

**Context.** CM3 (QR1.5) of the C-mirror sub-plan needs file/string sources and
sinks. The `STRING ( reference )` stream option (rule 105) routes list-directed
I/O to/from an in-memory character variable instead of SYSPRINT/SYSIN — the
`sprintf`/`sscanf` analogue — and was diagnosed unimplemented citing rule (105).

**Decision.** `PUT STRING(s) LIST(...)` writes the list-directed output into the
NONVARYING character variable `s`; `GET STRING(s) LIST(...)` reads it back. The
target must be a NONVARYING character variable (sema `checkStringTarget`;
VARYING, arrays, and PAGE/SKIP-with-STRING are diagnosed). Rather than a second
set of per-type output/input functions, the runtime keeps a selectable sink
(`out_buf`/`out_cap`/`out_len`) and source (`in_buf`/`in_len`/`in_pos`): `put_raw`
writes into `out_buf` when active and `next_char`/`get_token` read from `in_buf`
when active, so the existing `pli_put_list_*`/`pli_get_list_*` functions work
unchanged. Four new entries (`pli_string_put_open/close`, `pli_string_get_open/
close` in `pli_rt_abi.def`) switch the mode; `put_close` blank-pads the unused
tail of the target. `emitPut`/`emitGet` open the STRING before the item loop and
close it after (target addressed like an assignment left-hand side).

**Consequences.** `tests/core/string.pli` does a `PUT STRING` → `GET STRING`
round-trip and verifies the values are recovered; `tests/core/bad_string.pli`
rejects a non-character STRING target; `make test` and `make check` stay green.

**Rejected.** Adding a parallel `pli_put_str_*`/`pli_get_str_*` per-type function
set (duplicated the whole list-directed surface); routing through the FILE
option (needs file-handle state, QR2.5); supporting `STRING` with `PAGE`/`SKIP`
or a VARYING target (not meaningful for a fixed in-memory sink/source).

## ADR-068 — OPEN/CLOSE and the FILE ( f ) stream option

**Context.** CM3 (QR1.5) of the C-mirror sub-plan needs file sources and sinks —
the `fopen`/`fclose` analogue. `OPEN`/`CLOSE` (rules 100-103) and the `FILE ( f )`
stream option (rule 105) were diagnosed unimplemented; the `FILE` attribute
(rules 39,40) was not declared.

**Decision.** A `FILE`-declared variable (`DECLARE f FILE;`) carries no runtime
storage: its identity is a compile-time slot (0–15) assigned by sema. The runtime
keeps a fixed `FILE*` table (`pli_files[16]`) plus two current-stream globals
(`out_f`/`in_f`). `OPEN FILE(f) TITLE('name') [INPUT|OUTPUT|STREAM|PRINT]` calls
`pli_file_open(slot, name, len, mode)` (`fopen` with "r"/"w"); `CLOSE FILE(f)`
calls `pli_file_close(slot)`. `PUT FILE(f) LIST(...)`/`GET FILE(f) LIST(...)`
call `pli_put_select`/`pli_get_select` before the item loop and the matching
unselect after, so `put_raw`/`next_char` route through the file before
falling back to SYSPRINT/SYSIN — reusing the same per-type `pli_put_list_*`/
`pli_get_list_*` functions as the STRING sink/source (ADR-067). sema's
`checkFileTarget` resolves `FILE ( f )` to the symbol and requires it to be a
FILE variable; FILE and STRING are mutually exclusive per statement. OPEN without
a `FILE ( f )` option is diagnosed (this stage names one file).

**Consequences.** `tests/driver/file.sh` does a `PUT FILE` → `GET FILE` round-trip
against a relative filename in the gitignored test output directory and verifies
the values; `tests/core/bad_file.pli` rejects a numeric variable as a FILE target;
`make test` and `make check` stay green. `IDENT`/`LINESIZE`/`PAGESIZE`,
`RECORD`/`UPDATE`/`KEYED`/`ENVIRONMENT`, and record I/O stay M6.

**Rejected.** Giving each FILE variable real runtime storage (it is only ever the
compiler-resolved name of a slot here); a new `TK::File` (would force a case in
every type switch for a value that never reaches arithmetic); `FILE` with `PAGE`/
`SKIP` (not part of this stage's stream surface).

## ADR-069 — edit-directed `PUT/GET EDIT` with common format items

**Context.** CM3 (QR1.5) of the C-mirror sub-plan needs edit-directed transmission
(rule 108) — the `printf`/`scanf` analogue. `PUT [SKIP] [PAGE] LIST` existed, but
`EDIT`/`DATA` were diagnosed unimplemented; the format items (rules 44-55) had no
engine.

**Decision.** `PUT/GET EDIT ( datalist ) ( formatlist )` (the real-PL/I spelling;
the TR grammar's outer-parenthesis reading is OCR-ambiguous) is served for the
common items: the numeric `F(w,d)`, the character `A(w)`, and the control
`X(w)`, `SKIP(n)`, `PAGE`, and `LINE(n)`. The AST statement carries a flat
`formats` list (`FormatItem`/`HFormatItem`) alongside the existing `items`; each
data item is paired in order with the next `A`/`F` format while the control items
act between them without consuming data. `A`/`F` require a CHARACTER/numeric item
(respectively); for GET every item must be an assignable reference. IRGen walks
the format list with a data index, emitting `pli_put_edit_char`/`_fixed`/`_float`/
`_x`/`_skip`/`_page`/`_line` for output and `pli_get_edit_num`/`_char`/`_x`/
`_skip` for input; output routes through `put_raw` and input through `next_char`
so the STRING (ADR-067) and FILE (ADR-068) sources/sinks are honoured. A FIXED
value is rescaled from its stored `10^scale` representation to `d` fractional
digits (rounding half away from zero) before right-justification in width `w`;
GET `F(w,d)` parses the field to a double and the target conversion applies the
scaling.

**Consequences.** `tests/core/edit.pli` round-trips `F(w)`, `A(w)`, `X(w)`, and
`F(w,d)` through a STRING buffer and verifies the spacing content; `bad_edit.pli`
diagnoses the unimplemented `E` format and a GET data item that is not a
reference. `make test` and `make check` stay green; emitted IR shows the paired
`pli_put_edit_*`/`pli_get_edit_*` calls. `DATA`, `COPY`, `LINE` options,
format iteration `(n) (item)`, `E`/`B`/`C`/`P`/`COLUMN`/`R` items, a standalone
`FORMAT` statement, and a third `F` scale operand stay diagnosed (M5/D1).

**Rejected.** Reusing the list-directed `pli_put_list_*`/`pli_get_list_*`
functions (they separate and tokenize, not position in fixed-width fields); an
`E` scientific item in this slice (FLOAT uses `F`); an implicit-decimal-point
`F(w,d)` read (a field with an explicit `'.'` is parsed; the implied-decimal form
is not).

## ADR-070 — `E(w,d)` scientific format and structures by value

**Context.** Two CM tails of the C-mirror sub-plan. CM3 left `E` (and `B`/`C`/
`P`/`COLUMN`/`R`) diagnosed unimplemented by ADR-069; CM1 needed a whole
structure usable as an expression value — a structure-valued function
(`RETURNS` a structure), a structure argument passed by value, and a
structure-returning call assigned to a same-shape structure (rule 127).

**Decision (E-format).** `E(w,d)` is served for both output and input alongside
the ADR-069 items. Output converts the item to FLOAT and emits
`pli_put_edit_float_e`, which formats scientific notation with one leading digit,
`d` fractional digits, and a signed two-digit exponent (e.g. `1.25E+01`),
right-justified in width `w`. Input reuses `pli_get_edit_num` (its `strtod`
already parses the exponent form) and converts to the target. `E` requires a
numeric item (rule 53).

**Decision (structures by value).** A structure is a first-class value carried by
its address (`Val.ptr`). A structure-valued function is declared `RETURNS(NAME)`
where `NAME` is an enclosing structure variable whose shape the function takes
(a deep copy of its type, like a LIKE template); sema resolves it into `retTy`.
Its ABI is a hidden result pointer: the function returns `void` and the caller
allocates the result buffer, passes its address as the first argument, and a
`RETURN(struct)` copies the value into it — avoiding a by-value struct return in
the ABI. A whole-structure argument is passed BY VALUE: `argAddr` copies the
source storage into a fresh buffer, so the callee's writes never reach the
caller's structure (scalars and arrays stay by reference). Whole-structure
assignment accepts any structure-valued RHS (variable, minor-structure member,
or a structure-returning call).

**Consequences.** `tests/core/e_format.pli` round-trips `E(w,d)` through a STRING
buffer and checks the rendered content; `tests/core/struct_return.pli` exercises
a structure-returning function, a structure-valued assignment, and a by-value
argument. `make test` (149/149) and `make check` stay green; emitted IR shows the
hidden result buffer and the by-value `memcpy`. `RETURNS` of a non-structure name,
a dynamic-member structure, or a structure-returning function with `ENTRY`
statements are diagnosed. `DATA`, `COPY`, `LINE` options, format iteration,
`B`/`C`/`P`/`COLUMN`/`R`, a standalone `FORMAT`, and a third `F` scale operand
stay diagnosed (M5/D1).

**Rejected.** A true by-value struct return in the LLVM ABI (sret requires
changing the caller/callee return convention for one type); struct-returning
functions with `ENTRY` statements in this slice; inline structure definitions in
`RETURNS` (a declared template name is the supported form).

## ADR-071 — Recursive `%INCLUDE` before lexical analysis

**Context.** QR1.6 and CM4 require the safe subset of the C28-6571-3 Chapter 9
processor before replacement and conditional directives. A raw `%` previously
reached the language lexer and failed as an invalid character. Included text
must itself be scanned for includes, while apparent directives in PL/I comments
and character strings must remain source text.

**Decision.** A preprocessor stage runs before `Lexer`. It recognizes directives
only outside comments and character strings and implements `%INCLUDE` with one
member or path. Resolution starts in the containing file's directory, with a
`.inc` fallback for extensionless member names; quoted paths are accepted for
filesystem-oriented sources. Included text is recursively processed in place.
An active canonical-path stack rejects include cycles. Missing members,
malformed includes, and every other directive are diagnosed with a
C28-6571-3 Chapter 9 citation. Separate handler stubs provide the dispatch points
for declarations and replacement, activation, conditionals, loops, transfers,
and compile-time procedures.

**Consequences.** `tests/core/include.pli` exercises nested relative includes,
quoted and unquoted members, and ignored directives inside a comment and string.
`bad_include.pli` rejects a missing member, `bad_include_cycle.pli` rejects a
cycle, and `bad_preprocessor.pli` proves that an unimplemented `%IF` is
diagnosed by its stub rather than leaking into the lexer. The driver now feeds
the expanded source to diagnostics and the lexer.

**Rejected.** Treating include text as a lexer token stream (nested includes
must be expanded first); silently passing unsupported directives through;
implementing Chapter 9 replacement or control flow in this slice; include
search-path flags and the implementation-defined multi-identifier data-set form
before a concrete use case requires them.

## ADR-072 — Scalar math built-ins lowered through pli_* runtime wrappers

**Context.** The C-mirror sub-plan (CM5) maps the remaining QR2.7 Appendix 1
functions to C `<math.h>`. `FLOOR`, `CEIL`, `SQRT`, `EXP`, `LOG`, `SIN`, `COS`,
and `TAN` each take one numeric argument and yield a FLOAT value. The runtime
already links `<math.h>` (used by `pli_round`, `pli_mod_dd`).

**Decision.** Each built-in is typed in `Sema::typeBuiltin` (one numeric
argument, `FLOAT(6)` result, non-numeric argument diagnosed with rule (123))
and lowered in `IRGen::emitBuiltin` to a call of a thin `pli_*` wrapper in
`runtime/pli_rt.c`, registered in `pli_rt_abi.def` so the emitted IR ABI cannot
drift from the C ABI. `LOG` is the natural logarithm. The argument is converted
to FLOAT before the call.

**Consequences.** `tests/core/math.pli` checks each built-in against an expected
value within a small tolerance (`ABS` delta) and prints `PASS math`;
`bad_math.pli` proves a non-numeric argument is diagnosed with its rule number.
Inspection of `-emit-llvm` shows `double @pli_*(double)` calls and declarations.

**Rejected.** Emitting `llvm.*` math intrinsics directly or calling `math.h`
symbols from IRGen: both would bypass the ABI-def single source of truth and
add the only non-`pli_*` external surface. Complex component/conjugate and the
remaining Appendix 1 families stay for later CM5 slices.


## ADR-073 — Complex values as an `{double,double}` pair, expression-only

**Context.** The C-mirror sub-plan (CM5) starts QR2.2 complex work with the
component/conjugate operations `COMPLEX`, `REAL`, `IMAG`, and `CONJG`
(Appendix 1). These need a complex value to flow through expressions, but full
complex declarations, arithmetic, conversions, and I/O are out of this slice's
scope.

**Decision.** A new `TK::Complex` type kind represents a complex value as an
LLVM `{double,double}` struct (real, imaginary), mapped by `llvmTy`. A
materialised complex value is carried in a new `Val::cpx` field rather than the
scalar `reg`. `COMPLEX(a,b)` builds the pair with `insertvalue`; `REAL(z)` and
`IMAG(z)` extract a part with `extractvalue` as a FLOAT; `CONJG(z)` negates the
imaginary part (`fneg`) and rebuilds the pair. Constant operands fold away
through LLVM's optimizer. `TK::Complex` is not added to `isNumeric` and is not
wired into storage, conversion, assignment, arithmetic, or list-directed I/O
(PUT of a COMPLEX value is diagnosed, mirroring POINTER).

**Consequences.** `tests/core/complex.pli` checks each built-in within a small
tolerance and prints `PASS complex`; `bad_complex.pli` proves a non-complex
argument to `REAL` is diagnosed with rule (123). `-emit-llvm` on a runtime
(variable) operand shows the `insertvalue`/`extractvalue`/`fneg` sequence; the
constant case folds to a single `double`. A COMPLEX value cannot yet be stored
in a variable or written out, which stays for the QR2.2 complex type work.

**Rejected.** A full `COMPLEX` data attribute with storage/assignment this
slice (too large, and not the sub-plan's "start with" item); reusing `TK::Struct`
to fake a complex (semantically wrong and would invite struct-path confusion);
adding `TK::Complex` to `isNumeric` (complex is not a real arithmetic type for
the existing `+ - * /` operators).

## ADR-074 — COMPLEX as a storable data type with real↔complex conversion

**Context.** The C-mirror sub-plan (CM5) completes the complex type begun in
ADR-073: `COMPLEX` should be usable as a declared variable, not just a transient
expression value. TR 25.084 rules (14),(15) list `COMPLEX` among the data
attributes, and the pair must be assignable and read back.

**Decision.** `COMPLEX` is a data attribute accepted by the declaration parser
(`AttrBag.complex`, conflicting with any other data attribute under rule (15))
that yields `Type::complexTy()`. IRGen wires the `{double,double}` pair through
storage and assignment: `loadSym` loads it into `Val::cpx`, `storeScalarTo`/
`storeTo` store it, and `convert` implements the real↔complex rules (complex→
complex passes through, complex→real takes the real part, real→complex sets a
zero imaginary part). `Sema::checkAssignable` allows these same conversion pairs
so assignment type-checks match the codegen.

**Consequences.** `tests/core/complex_var.pli` declares complex variables,
assigns a `COMPLEX(a,b)` value, assigns a real (imag part 0), assigns complex to
a real variable (real part), and reads back with `REAL`/`IMAG`/`CONJG`;
`bad_complex_var.pli` proves COMPLEX combined with another data attribute is
rejected under rule (15). `-emit-llvm` shows `store { double, double }`/
`load { double, double }` and `insertvalue`/`extractvalue` sequences. Complex
arithmetic (`+ - * /`), imaginary constants, and complex list-directed I/O stay
unimplemented (PUT of a COMPLEX value is diagnosed).

**Rejected.** Adding `TK::Complex` to `isNumeric()` (would make the existing
arithmetic operators treat a complex pair as a single real scalar and
miscompile); implementing complex arithmetic in this slice (a separate, larger
effort, out of the "declare, assign, convert" scope).

## ADR-075 — Mixed void/function ENTRYs: one common impl type, no fall-through

Context. ADR-026 shares one implementation function across a procedure's
primary entry and its ENTRY statements, returning one type: every
function-valued entry point (the procedure's own RETURNS and each ENTRY's
RETURNS, rule (34)) must agree on it, and a truly mixed return type is
diagnosed. Two gaps remained: a void (non-function) primary coexisting with
function ENTRYs had no specified shape, and the impl's segments fell through
into each other in body order, so a void primary segment could execute a
function segment's code (or emit a `ret void` inside a valued function).

Decision. The shared impl returns the single result type shared by every
function-valued entry point (void when none); a void primary may coexist with
function ENTRYs (`entry_mixed.pli`). The type agreement is enforced at both
ends: sema rejects a plain RETURN when the impl's common type is non-void and
a RETURN(value) when it is void (rules (56),(81); `bad_entry_ret.pli`), and it
tracks the current ENTRY segment through nested Group/Begin/DO bodies. Codegen
never falls through: each segment closes with a branch to its own return pad,
and each pad returns the impl's type. A RETURNS attribute on a non-ENTRY
DECLARE item is diagnosed (rule (34); `bad_returns_attr.pli`).

Consequences. `entry_mixed.pli` covers a void primary beside two function
ENTRYs plus an external ENTRY...RETURNS function call; the linkage (`.c`
helper, cross-unit link) reuses the existing `cinterop` pattern. Truly mixed
valued types stay diagnosed; USES/SETS/REDUCIBLE entry attributes stay
out of scope.

## ADR-076 — ON ERROR: handler-id stack, frameless units, ONCODE 1/0

Context. ADR-009 specifies the M4 condition mechanism (runtime handler stack,
units compiled to functions taking the establishing frame, compile-time
enable-state). The first served slice is ERROR only with the ONCODE code, and
it must not disturb programs that establish no handlers.

Decision. The runtime owns a stack of handler ids (0 means the SYSTEM action;
an empty stack behaves the same). Each established ON-unit gets a dense
1-based id and its own internal `void()` function; SIGNAL reads the top id
and either aborts (system action, exit 8 via the existing ERROR path) or
calls the handler with `pli_set_oncode(1)` around it, then resumes after the
SIGNAL. ONCODE() reads the runtime value: 1 inside a SIGNAL-raised unit, 0
elsewhere. Units run without the establishing frame, so sema diagnoses
automatic-variable access (including DO control variables), RETURN, nested
ON, DECLARE, ENTRY, calls needing static links (diagnosed at codegen, where
envs are known), and GO TO in an ON-establishing procedure. Scoping is
dynamic: procedure entry saves the depth and every RETURN plus fall-through
restores it; BEGIN blocks restore on exit; REVERT pops (a pop on an empty
stack is a no-op). When the module establishes no handlers, no condition code
is emitted at all. SNAP parses with a no-effect warning. Non-ERROR conditions
are diagnosed at parse time with their rule numbers (`bad_on_cond.pli`).

Consequences. `on_error.pli` (golden) covers establish/raise/resume, ONCODE
inside and out, re-establishment after REVERT, and the SYSTEM/REVERT no-op
path. Frame-carrying units, computational and I/O conditions, FINISH, and
non-local GO TO unwinding stay diagnosed for later slices.

## ADR-077 — %REPLACE: token-level substitution as a marked extension

Context. TR 25.084 defines no preprocessor: the only comment form is
`/* ... */` (rules (149)-(151)), and `%` is not a source character.
`%REPLACE` is requested anyway as an important directive, so it is served
as a documented dialect extension, not as spec conformance (it also lifts
the ARCHITECTURE non-goal #2 bar for this one directive only).

Decision. `%REPLACE name BY <tokens> ;` is expanded in the lexer after the
full token stream is built and before parsing: every later `Word` matching
the name is replaced by copies of the collected tokens, stamped with the
use-site location. One left-to-right pass: a use sees only earlier
directives, and spliced tokens are not re-expanded, so self-reference
terminates. Strings and comments never surface as words, so their contents
are unaffected. Only `%REPLACE` is served; any other `%` directive and any
malformed directive (missing name, missing BY, empty replacement) is
diagnosed, citing ADR-077 instead of a TR rule number. The directive's own
`;` terminates the directive and is not part of the replacement text.

Consequences. `replace.pli` covers single-token (array bound, value) and
multi-token (whole statement) substitution plus string immunity;
`bad_replace.pli` covers non-REPLACE directives. Redefinition takes the
last definition. `%INCLUDE`, `%IF`, `%DECLARE`, and recursive expansion
stay out of scope.

## ADR-078 — %INCLUDE search paths: file dir, -I, env, exe-relative default

Context. ADR-071 resolves `%INCLUDE` only against the including file's
directory, so shared snippet libraries need absolute paths baked into
sources. Clang solves the same problem with an ordered search (-I, CPATH,
built-in defaults) plus `-v` visibility.

Decision. Search in order: the including file's directory (unchanged),
repeatable `-I` (`-I dir` and `-Idir`, first wins), colon-separated
`PLIC_INCLUDE_PATH` (CPATH-like, skipped when empty), then the
executable-relative `share/plic/include` default (mirroring the runtime-lib
fallback idiom; a missing default simply never hits). `-v` prints the
configured dirs. No `-isystem`/`-iquote` split: one ordered list is enough
for snippet libraries.

Consequences. `driver/include_dirs` covers -I discovery, first-wins order,
env discovery, missing-file rejection, and the -v default listing.

## ADR-079 — Small binaries: sectioned runtime archive + link-time strip

Context. `libpli.a` is a single object, and static archives link at object
granularity, so any runtime reference pulled all of `pli_rt.c` into every
binary: a hello-world carried 85 runtime functions. Splitting the source
per area was rejected as churn against active feature work.

Decision. Compile the runtime with `-ffunction-sections -fdata-sections`
and always pass the platform strip flag on the link step (`-dead_strip` on
macOS, `--gc-sections` elsewhere; `--release` already stripped). Hello now
keeps exactly its reachable set (`pli_rt_init/fini`, `put_skip`,
`put_list_char`).

Consequences. `driver/link` asserts an unused probe (`pli_sin`) is absent
and a used one present. A future source split composes with this unchanged.

## ADR-080 — Programmer-named conditions: use-declared names, tagged dispatch

Context. Rule (99) names conditions as `CONDITION (identifier)`, but no TR
production declares the name (rule (15) has no CONDITION attribute), so
names are use-declared in first-use order, consistent with implicit
declarations (ADR-011). Dispatch must isolate conditions while sharing
block scoping with ERROR.

Decision. Sema registers each name program-wide (key = order + 1, key 0 is
ERROR) and rejects names that collide with a declared variable, parameter,
or procedure, and `CONDITION(ERROR)`. The runtime stack carries tagged
(key, id) entries: SIGNAL runs the topmost handler for its own condition
(or aborts when none), REVERT drops the topmost entry for its condition
(no-op when none), and depth/reset scope both conditions uniformly.
Handlers are per-(key, id); only ERROR touches ONCODE, so units observe
state through globals/output.

Consequences. `on_cond.pli` (golden) covers per-condition dispatch beside
ERROR, resume, and re-establishment; `bad_on_cond_decl.pli` and
`bad_on_cond_error.pli` pin the two diagnostics. Computational, I/O,
FINISH, AREA, and CHECK conditions stay diagnosed.

## ADR-081 — Minimal DISPLAY: one scalar plus a newline

Context. Rule (114) is `DISPLAY ( expression )` (Y33 attests a scalar print
and a REPLY form). The runtime terminates lines lazily (a `col` counter plus
`fini`), while DISPLAY must end its own line eagerly.

Decision. Serve `DISPLAY (scalar)` for fixed/float/char/bit via four
`pli_display_*` runtime functions sharing the list-directed value formats;
array/struct operands are diagnosed by sema, POINTER/COMPLEX at codegen
(mirroring PUT LIST), and `REPLY` stays diagnosed. Each call starts on a
fresh line (ending a pending PUT line first, as SKIP does) and resets the
column/item state, so mixed PUT/DISPLAY output never joins or splits lines.

Consequences. `display.pli` (golden) covers all four scalar types;
`bad_display.pli` pins the non-scalar diagnostic. `REPLY` is a later slice.

## ADR-082 — Quoted %REPLACE operands are re-scanned as source text

Context. Iron Spring writes replacements as double-quoted strings
(`%replace X by "...";`), but `"` is not a plic source character. The
quoted form must work without making `"..."` legal elsewhere.

Decision. The lexer produces a dedicated `DqString` token (`""` escapes a
quote, as with `'`). A replacement that is exactly one `DqString` is
re-lexed as source through a fresh lexer over the contents (locations
stamped at the directive; nested directives are not expanded, keeping the
single-pass guarantee); anything mixing quotes with raw tokens is
diagnosed. The parser rejects a stray `DqString` with an explicit
diagnostic instead of the generic expression error.

Consequences. `replace_quoted.pli` (golden) covers quoted single-token,
quoted multi-token, and unquoted coexistence; `bad_replace_mixed.pli` and
`bad_dqstring.pli` pin the two diagnostics. Unquoted behavior is unchanged.

## ADR-083 — Preprocessor conditionals: %IF over integer variables

Context. CM4 needs conditional compilation (`#if` analogue); the Chapter 9
stubs reject everything past `%INCLUDE`. Literals-only conditions would
prove machinery without utility, so minimal integer variables ship in the
same slice.

Decision. `%DECLARE a, b;` declares integer variables (default 0;
redeclaration keeps the value, so double inclusion is idempotent);
`%X = expr;` assigns (undeclared target and bare `%X` diagnosed).
`%IF expr %THEN directive [%ELSE directive]` evaluates literals, vars,
parens, unary minus, `+-*/` (div-by-zero diagnosed), comparisons, and
`& | ¬` (all not-sign spellings) to select exactly one arm; arms are full
directives run or structurally skipped recursively, so nesting, conditional
`%INCLUDE`, and conditional `%REPLACE` text all work. Skipped arms have no
effects and raise no file errors; newlines stay preserved for diagnostics.
`CHARACTER` variables, `%ACTIVATE`, and the other stubs stay diagnosed.

Consequences. `pp_if.pli` (golden, with `pp_yes.inc`/`pp_no.inc`) covers
taken/untaken/else/nested arms; `bad_pp_if.pli` pins the undeclared-name
diagnostic. `%DO` groups (multi-directive arms) stay out per the subplan.

## ADR-084 — Complex arithmetic over pairs, exact equality, imaginary constants

Context. CM5 serves complex values as {double,double} pairs with REAL/IMAG/
COMPLEX/CONJG already in place, but operators were missing: arithmetic
diagnosed complex operands, `2i` was rejected at lex time, and comparisons
silently compared real parts only.

Decision. `+ - * /` over complex-or-numeric operands yield COMPLEX (mixed
reals convert with a zero imaginary part through the existing conversion);
`/` divides by c^2+d^2 inline; `**` on complex stays diagnosed. `=`/`^=`
compare part-wise exactly; ordered comparisons are diagnosed. An imaginary
constant `2i` is a complex value with a zero real part (fractional and
binary-radix forms reuse the existing value parsing).

Consequences. `complex_arith.pli` covers all four operators, mixed reals,
imaginary constants, and both equalities; `bad_complex_power.pli` and
`bad_complex_cmp.pli` pin the two diagnostics. Complex I/O and remaining
Appendix-1 math stay out. Bare factored `a, b <attr>;` typing only the
last item is a known pre-existing parser limitation, untouched here.

## ADR-085 — Complex output as real, sign, imaginary, I

Context. CM5 values exist with arithmetic served, but output was diagnosed
while input parsing (`4+6I` back into a pair) is a separate string-parsing
feature. List-directed input of complex also slipped past sema to a codegen
backstop, invisible to `-fsyntax-only`.

Decision. `PUT LIST` and `DISPLAY` print complex as `%.6g%+.6gI` (real,
sign, magnitude, I) through `pli_put_list_complex` / `pli_display_complex`,
the latter framed by the DISPLAY line discipline. `GET LIST` of complex is
diagnosed in sema beside the struct case, so front-end checks reject it.

Consequences. `complex_io.pli` (golden) covers single/multi-item PUT and
DISPLAY including negative parts; `bad_get_complex.pli` pins the input
diagnostic. Complex input parsing stays out.

## ADR-086 — Complex input as re+imI tokens through element pointers

Context. Output serves `re+imI`; input was diagnosed in sema only, after
slipping past it to a codegen backstop invisible to `-fsyntax-only`.

Decision. `pli_get_list_complex` parses one token: an optional trailing
`I` selects complex shape, split at the last interior sign that is not an
exponent marker (so `1e-3+2I` works); no `I` means a bare real with zero
imaginary part; missing or malformed parts read as zero, mirroring the
other lenient readers. IRGen reads through element pointers into a pair
alloca and loads it, reusing the normal store path; the sema diagnostic
is lifted (the codegen backstop stays as defense).

Consequences. `driver/get_complex` covers both shapes plus the bare-real
rule; the transient `bad_get_complex.pli` pin is removed.

## ADR-087 — ASIN, ACOS, ATAN2, CBRT through thin runtime wrappers

Context. The Appendix-1 math family stops at ATAND, leaving the remaining
C counterparts ASIN, ACOS, two-argument ATAN2, and CBRT unserved.

Decision. ASIN/ACOS/CBRT join the one-argument table (numeric in, FLOAT
out); ATAN2 gets its own two-numeric-argument block in C order (y, x),
mirroring the one-arg shape otherwise. All four lower through thin
`pli_*` wrappers over C99 `asin`/`acos`/`atan2`/`cbrt`, like the family.

Consequences. `math4.pli` checks all four within tolerance;
`bad_math4.pli` pins arity and numeric diagnostics.

## ADR-088 — Decimal I/O prints and reads the 10^q point

Context. FIXED DECIMAL values store integer x 10^q, but PUT printed the
raw integer (`12.5` as `1250`) and GET read it back unscaled: silently
wrong output with no test coverage.

Decision. New `pli_put_list_decfixed` / `pli_display_decfixed` print
exactly q fraction digits (the DISPLAY form framed by the line
discipline); `pli_get_list_decfixed` scales the token (truncating beyond
q, lenient like the other readers). IRGen routes scaled FIXED DECIMAL
through them; the GET value is truncated to the target width without
rescaling so the store passes it through (an i64-into-narrow store
miscompiled this once during development).

Consequences. `decimal_io.pli` (golden) and `driver/get_decimal` cover
output, DISPLAY, and round-trip input. Overflow checks stay out (D1/QR2).

## ADR-089 — FIXED BINARY overflow traps to hard ERROR

Context. QR1.2 needs overflow checks, but routing them needs a SIZE
condition (QR1.4). Wraparound was silent. The SUBSCRIPTRANGE interim
(M2) sets the precedent: check now, hard error now, routable later.

Decision. `+ - *` over FIXED BINARY lower through overflow intrinsics
(width-generic via bit width) and unary minus guards INT_MIN; a set flag
branches to `pli_fixed_overflow`, which raises through the ERROR path so
a future ON ERROR/SIZE already observes it. FLOAT, BIT-modular, and
DECIMAL-precision arithmetic are untouched. `TRUNC`/`MOD` sdiv edges and
decimal precision overflow stay follow-ups; condition prefixes will gate
the checks in QR1.4.

Consequences. `driver/overflow` covers all four traps plus in-range
edges; the `tests/ir` arith/func expectations track the checked shape.

## ADR-090 — Dynamic lower-bound params read both bounds at entry

Context. QR1.1 serves single-axis `A(lb:ub)` locals (ADR-058) and `x(k)`
params (ADR-054), but `x(l:u)` was diagnosed: the convention was thought
to convey only the upper bound, so a lower-bound param would index from
the wrong origin.

Decision. No ABI change. The bound exprs name caller-supplied params
already passed by reference, so the callee evaluates both at entry into
`dynUb_`/`dynLb_` (mirroring locals). Indexing is remap-by-position:
`x(l)` addresses the caller's first element, with per-axis
SUBSCRIPTRANGE and `LBOUND`/`HBOUND`/`DIM`/reductions over the live
bounds. Multi-axis dynamic params stay diagnosed.

Consequences. `dyn_param_lower.pli` covers l=1, remapped l=0/2/5,
writes-through, and a function result; `bad_dyn_param.pli` keeps only
the multi-axis case and `bad_dyn_lower.pli` the beyond-first-axis
lower case.

## ADR-091 — Whole-structure copy with dynamic members deep-copies

Context. QR1.1 serves whole-structure copy for fixed shapes by storage
`memcpy` (rule 127), but a struct holding a dynamic-array member stores
a buffer pointer: `memcpy` would alias the source buffer, so a later
write through either side would corrupt the other. The copy was
diagnosed rather than miscompiled.

Decision. Plain assignment `t = s` (identical shapes) deep-copies:
save each target buffer pointer, `memcpy` the storage, restore the
pointers, then `memcpy` each buffer's contents positionally (lower
bounds may differ; only the extent matters). Live extents are compared
first and a mismatch traps through `pli_subscript_oob` rather than
overflowing or silently truncating. Non-`VarRef` struct sources are
diagnosed. `LIKE`, `RETURNS`, by-value arguments, and `BY NAME` stay
diagnosed follow-ups (QR2.1); struct-typed call-argument checking for
nested callees is a known hole (params resolve after callers type-check,
so the check is skipped) and needs its own fix.

Consequences. `struct_dyn_copy.pli` covers scalar/element copy,
`LBOUND`/`HBOUND`/`DIM`/`SUM` over the copied member, and
deep-not-aliased divergence; `bad_struct_dyn.pli` now pins the
`LIKE`-with-dynamic-member diagnostic.

## ADR-092 — INITIAL on structures with a trailing dynamic member

Context. QR1.1 serves `INITIAL` on dynamic arrays (ADR-061) and on
fixed-member structures (ADR-057), but a struct-level itemlist covering
a dynamic member was rejected with a misleading leaf-count diagnostic:
the member's runtime extent counted as one leaf. Member-level
`INITIAL` (on a `2`-level item) is silently dropped, for fixed members
too — a separate pre-existing hole left untouched here.

Decision. Only a trailing top-level dynamic array member with scalar,
non-`CHARACTER` elements is served: the flat itemlist (already
flattened for iteration factors, `*`, groups) fills the static leaves
first with the usual count check ("too few" diagnosed), then one buffer
element per remaining value with no count check (the extent is
runtime), each folded against the element type. Codegen stores the
statics to their fields and the remainder straight-line into the loaded
member buffer, which pass 3 pre-sizes by the whole itemlist length (an
over-approximation — only trailing values target the member — mirroring
the ADR-061 pre-size). Any other dynamic layout (non-trailing, nested,
struct/`CHARACTER` elements) is diagnosed with rule (26), never
silently misfilled. The shape walk tests each top-level member for
being or holding a dynamic array directly, since `hasDynamicMember`
only sees dynamics nested inside its argument.

Consequences. `struct_dyn_init.pli` covers full/short/iterated lists,
bounds inquiries, per-variable buffers, and reactivation;
`bad_struct_dyn_init.pli` pins the non-trailing and nested
diagnostics.

## ADR-093 — BY NAME copies dynamic members by buffer contents

Context. QR1.1 serves `BY NAME` across differing layouts (ADR-043) and
plain whole-structure deep copy (ADR-091), but a same-named dynamic
member was storage-copied: the field holds a buffer pointer, so the
target aliased the source buffer and later writes bled across. Sema
already requires identical array types for `BY NAME` array members, so
the mismatch shape was diagnosed (rule (86)) and only the identical
shape miscompiled.

Decision. Thread each side's symbol and root field path through
`emitByNameCopy` (layouts may differ, so paths extend independently on
recursion) and copy a matched dynamic member's buffer contents
positionally, never the pointer field. Live extents compare first with
a mismatch trap through `pli_subscript_oob` (same convention as
ADR-091); unresolvable buffers are diagnosed with rule (13). No sema
change was needed. Fixed members keep their paths untouched.

Consequences. `struct_by_name_dyn.pli` covers cross-layout matching,
`SUM` over the copied member, skip-on-absent both ways, and
deep-not-aliased divergence; `bad_by_name_dyn.pli` pins the
dynamic-vs-fixed pairing diagnostic.

## ADR-094 — Callee parameters resolve before any body is typed

Context. Argument checks (`checkAssignable` on `CALL` and function-call
arguments, rules (34),(78)) read the callee's `paramSyms`, but those
resolved in `processProc` — i.e. in procedure order. A caller typed
before its callee saw an empty descriptor list and skipped every check,
so e.g. a whole-structure value with a dynamic member passed by value
to a nested procedure compiled cleanly into a pointer-aliased
`memcpy` (the ADR-091 follow-up hole).

Decision. Resolve every procedure's own and `ENTRY` parameters in pass
1b right after its declarations are collected; `processProc` reuses the
same `resolveProcParams` helper, which is a no-op when the lists are
already populated (additionally guarded inside `resolveParams`). No
diagnostic wording or accepted surface changes otherwise — previously
silent mismatches now diagnose, previously valid calls type-check
identically.

Consequences. `bad_struct_dyn.pli` gains the by-value-argument case
alongside its `LIKE` case; the full suite stays green (201/201), so no
passing test relied on the hole.

## ADR-095 — FIXED DECIMAL overflow traps to hard ERROR

Context. QR1.2 serves FIXED DECIMAL storage, scaled arithmetic, I/O,
and comparison (ADR-006/056/088), and ADR-089 traps FIXED BINARY
`+ - *`/negation while explicitly leaving DECIMAL precision overflow
wrapping silently: `999.99 + 0.01` into `(5,2)` printed `1000.00`, and
narrowing conversions truncated. Full precision conformance (widened
intermediates, exact result precisions) is D1/QR2 scope, not this slice.

Decision. Two complementary checks through the existing
`pli_fixed_overflow` hard-ERROR path (routable by a future SIZE, same
as ADR-089): binary `+ - *` and the `MULTIPLY` builtin use width-checked
ops for decimal intermediates too (a wrapped intermediate always
exceeds any declared digits); narrowing conversions trap past the
target — `10^prec` digits for a `FIXED DECIMAL` target at prec 18 or
below (any i64 fits wider targets, so they skip), storage width for a
32-bit `FIXED BINARY` target — with statically fitting values
(same-or-wider decimal source after rescale) skipping the check and
scale-up rescaling itself overflow-checked so the magnitude check never
reads a wrapped value. Unary minus needs no guard: every storable
decimal magnitude fits its width, so negation cannot wrap.

Consequences. `driver/decimal_overflow` covers add/sub/mul/convert
traps plus fitting edges (rescaled narrowing, decimal-to-binary);
`decimal.pli` and `decimal_io.pli` stay green unchanged. Known
limitation: an intermediate wider than its storage width traps even
when the final target could hold the value — serving that needs the
deferred widened intermediates.

## ADR-096 — FLOAT to FIXED conversions trap outside the range

Context. QR1.2 traps FIXED arithmetic and narrowing overflow (ADR-089,
ADR-095), but `FLOAT -> FIXED` converted via a bare `FPToSI`, which is
UB outside the destination range: `1.0e20` into `FIXED BIN(31)`
silently produced garbage. `FIXED -> FLOAT` needs no check (every i64
is representable, approximately).

Decision. Check the float domain before converting, through the same
`pli_fixed_overflow` hard-ERROR path: binary targets use a closed lower
bound (so exact `INT_MIN` stays storable) with an open upper bound;
decimal targets use open digit bounds at prec 18 or below and the i64
bounds above (the conversion itself goes through i64). Ordered
compares make NaN trap as well. No sema change was needed.

Consequences. `driver/float_fixed_overflow` covers binary-32,
binary-64, and scaled-decimal aborts plus fitting edges (exact
`INT32_MIN`, `±999.99`); `decimal.pli` stays green unchanged. While
landing this, the runner's timeout path (which crashed the whole suite
on one slow job instead of failing it) was fixed, the overflow message
generalized from `FIXED BINARY overflow` to `FIXED overflow`, and
`driver/decimal_overflow` dropped its redundant subtraction case to
 stay comfortably inside the per-job timeout under parallel load.

 ## ADR-097 — SIZE routes fixed-overflow traps with abort fallback

 Context. QR1.4 needs recoverable conditions, and ADR-089/095/096 leave
 every fixed-overflow trap (binary `checkedArith`, decimal `magTrap`,
 float-to-fixed `floatRangeTrap`, neg-`INT_MIN`) on the abort path
 `pli_fixed_overflow` pending a SIZE condition that can route it.

 Decision. SIZE is a builtin with fixed key -1 (0 is ERROR, >= 1 are
 rule (99) names in first-use order), so it never collides with user
 conditions; `CONDITION(SIZE)` is diagnosed like `CONDITION(ERROR)`.
 `ON`/`REVERT`/`SIGNAL SIZE` reuse the generic keyed push/top/pop and
 per-(key, id) handlers (`PLI_ON_SIZE_n`); `SIGNAL SIZE` without a
 handler aborts like ERROR. Each trap site calls `emitSizeTrap(okBB)`:
 with no ON SIZE in the module it keeps the unconditional abort call
 (no IR change for existing programs); otherwise it checks
 `pli_on_top_cond(SIZE)` — empty aborts, established switches to the
 handler and resumes at `okBB` with the wrapped value. Block/procedure
 scoping reuses the shared stack depth/reset, so SIZE pops with ERROR.
 Only ERROR touches ONCODE in this stage.

 Consequences. `on_size.pli` covers establish/raise/resume,
 re-establishment, and a binary-overflow recovery
 (`2147483647 + 1` resumes as `-2147483648`); `bad_on_cond_size.pli`
 pins the `CONDITION(SIZE)` diagnostic; `driver/overflow`,
 `driver/decimal_overflow`, and `driver/float_fixed_overflow` still
 abort unhandled. Known limits: resume continues with the wrapped
 value, SIZE sets no ONCODE, and remaining computational conditions
 stay diagnosed.

 ## ADR-098 — PUT DATA output as NAME=value pairs

 Context. QR1.5 needs data-directed transmission (rule (106)); only
 list-directed and edit-directed PUT exist. GET DATA input (matching
 `NAME=value` pairs back to variables) is the larger half; output alone
 is independently useful (state dumps, golden-testable) and shares the
 SKIP/PAGE/FILE/STRING routing with LIST.

 Decision. `PUT DATA(a, ...)` parses like a LIST datalist with a `data`
 flag carried through HIR; sema requires plain scalar variable
 references (names must be printable), diagnosing constants,
 subscripted/qualified references, and non-scalar types. IRGen emits
 each name as a compile-time global via `pli_put_data_name` (", "
 between items, "=" after each name) then the value through the
 existing list-directed printer, closing with `pli_put_data_end`
 (";"). The runtime suppresses the value's blank separator after a
 name via a `data_value_next` flag, so FILE/STRING sinks keep working
 through `put_raw`.

 Consequences. `put_data.pli` covers FIXED/FLOAT/CHAR/BIT output
 (`A=42, X=2.5, S=ab  , B=1;`; CHAR keeps its blank padding, as in
 LIST); `bad_put_data.pli` pins the non-variable diagnostic. Known
 limits (see ADR-099 for the input half): subscripted/qualified items
 and non-scalar types stay diagnosed.

 ## ADR-099 — GET DATA input matches NAME=value pairs in any order

 Context. ADR-098 serves PUT DATA output; the input half (rule (106))
 must read `NAME=value` pairs back, in any order and skipping unknown
 names, through the same SYSIN/FILE/STRING sources as GET LIST.

 Decision. `GET DATA(a, ...)` parses like PUT DATA with the same sema
 shape rule (plain scalar variable references only). IRGen emits a
 runtime-driven pair loop: `pli_get_data_next` returns each uppercased
 NAME length (0 at `;`/EOF, consuming the terminator; malformed pairs
 without `=` are skipped), a per-item `pli_data_name_is` chain stores
 into the matching variable through the existing typed list-directed
 readers and `storeGetTarget`, and `pli_get_data_skip` discards unknown
 names' values. `get_token` also terminates at `;` (raising `tok_semi`
 for the following pair call); numbers already tolerate the prefix
 parse, so GET LIST behaviour is unchanged.

 Consequences. `get_data.pli` covers a PUT/GET round-trip over
 FIXED/FLOAT/CHAR/BIT plus out-of-order pairs with an unknown name
 skipped; `bad_get_data.pli` pins the non-variable diagnostic. Known
 limits: subscripted/qualified items and non-scalar types stay
 diagnosed, as do `COPY`/`LINE` options.

 ## ADR-100 — Static recursion cycles require RECURSIVE on every member

 Context. Rule (5) carries a `RECURSIVE` procedure option, but the
 parser accepted and discarded it, so direct and mutual recursion
 compiled with or without it. Codegen already gives each activation
 its own AUTOMATIC storage, so recursion happens to work; the gap is
 conformance, not lowering.

 Decision. Record `RECURSIVE` on the AST `Proc` (mirrored to HIR and
 shown by `--print-hir`) and check it in sema after all bodies are
 typed, when `CALL` and function-reference callees are resolved. Build
 the static call graph (a call through an entry-namelist alias or an
 `ENTRY` name maps to the same owning `Proc`, so it is still a
 self-edge; `INITIAL CALL`, bound/format expressions, and `ON`-unit
 bodies count as edges) and report one error at the procedure
 definition of each cycle member lacking the attribute, citing rule
 (5). Every procedure in a cycle must carry `RECURSIVE`, since each
 activation is re-entered while still live.

 Consequences. `recursive.pli` covers direct, mutual, `CALL`, and
 per-activation AUTOMATIC recursion with the attribute present;
 `bad_recursive.pli` pins the direct diagnostic and
 `bad_recursive_mutual.pli` the mutual one; `func.pli`,
 `multientry.pli`, `staticlink.pli`, and `ir/func.pli` now carry
 `RECURSIVE`. Known limits: calls across translation units cannot be
 seen and stay unchecked, and `STATIC` storage keeps its existing
 AUTOMATIC treatment across recursion.

 ## ADR-101 — Sequential RECORD files transfer fixed-size binary records

 Context. Rules (112),(113) record I/O stood fully diagnosed (`diag →
 M6`), so files were stream-text only; full record I/O (sequential,
 direct, keyed, buffering, file status) is QR2.5 scale. The smallest
 change that proves file processing beyond text streams is one
 organisation end to end.

 Decision. Serve the SEQUENTIAL record form only: `OPEN FILE ( f )
 RECORD SEQUENTIAL [INPUT|OUTPUT] TITLE ('name')` opens a binary file
 (`wb`/`rb` on the existing FILE slot table via `pli_file_open_record`);
 `WRITE FILE ( f ) FROM (v)` appends one record, `READ FILE ( f ) INTO
 (v)` consumes one, with new `Read`/`Write` AST/HIR kinds flowing
 through the usual chain. FROM accepts any scalar value expression
 (only the value is read, as with DISPLAY); INTO requires a plain
 scalar variable (it needs storage, as with PUT DATA). The image is
 FIXED 8B, FLOAT 8B, BIT(1) 1B, CHAR(n) nB in host byte order
 (implementation-defined). A use of a closed slot, a failed transfer,
 or a short READ raises ERROR in the runtime, since ON ENDFILE stays
 diagnosed (97); cross-mode misuse (stream verbs on a record slot and
 vice versa) is unchecked in this slice.

 Consequences. `driver/record` round-trips FIXED (incl. negative),
 FLOAT, CHAR, and BIT through a RECORD SEQUENTIAL file;
 `bad_record.pli` pins the REWRITE/DELETE diagnostic and
 `bad_record_into.pli` the INTO-variable diagnostic. Known limits:
 REWRITE/DELETE/LOCATE/UNLOCK, IGNORE/KEYTO/KEY/NOLOCK/SET/KEYFROM/
 EVENT, keyed/direct organisations, RECORD+STREAM, and ON ENDFILE
 stay diagnosed for the M6/QR2.5 remainder.

 ## ADR-102 — Recoverable SUBSCRIPTRANGE and ZERODIVIDE traps

 Context. Only ERROR/SIZE/`CONDITION(name)` dispatched through the
 handler stack; a subscript slip died in `pli_subscript_oob` with no
 handler, and a zero divisor died downstream as a misleading FIXED
 overflow (`7/0` is float division yielding inf, which then fails the
 float-to-fixed conversion). Both conditions are rule (94) and share
 the SIZE establish/raise/pop mechanics, so they plug into the same
 keyed dispatch.

 Decision. New fixed keys (-2/-3) served in the parser (which now
 rejects them in `CONDITION()`), resolved in sema, and dispatched in
 irgen through a shared `emitCondTrap` (the SIZE emitter is now a
 wrapper with identical output). A trapped SUBSCRIPTRANGE on an
 index check resumes with the index clamped into range
 (implementation-defined; the guarded computation uses the clamped
 index); by-name extent mismatches notify the handler, then abort,
 since no index exists to resume with. A trapped ZERODIVIDE (`/`,
 DIVIDE, MOD over a zero divisor) resumes with 0
 (implementation-defined). Unhandled traps keep the old aborts
 (`pli_subscript_oob`, new `pli_zerodivide`), so code without these
 ON-units is unchanged apart from two selects per subscript axis and
 one compare per division. Float `/` by zero now traps instead of
 yielding inf, and complex division by zero is untouched.

 Consequences. `on_subscriptrange.pli` covers SIGNAL plus fixed and
 dynamic read/write slips; `on_zerodivide.pli` covers SIGNAL plus
 `/` (float and fixed targets) and MOD; `bad_on_cond_subrange.pli`
 and `bad_on_cond_zerodivide.pli` pin the `CONDITION()` exclusions,
 and `bad_on_cond.pli` now uses OVERFLOW. Known limits: complex
 division by zero, CONVERSION/FIXEDOVERFLOW/OVERFLOW/UNDERFLOW/
 STRINGRANGE/AREA/FINISH and the I/O conditions stay diagnosed.

 ## ADR-103 — External linkage for top-level PL/I procedures

 Context. Every procedure was emitted with internal linkage under a
 `PLI_`-prefixed name, so a cross-module `ENTRY...EXTERNAL`
 reference (rules (34),(38)) could only resolve against C: linking
 two PL/I units failed with undefined `_NAME`. The documented model
 (ARCHITECTURE name mangling) already promises that EXTERNAL
 procedures keep their upper-cased PL/I name.

 Decision. A top-level non-MAIN procedure is externally linked under
 its upper-cased name (already upper-cased by the lexer); the MAIN
 procedure keeps its module-private name since only its own `main`
 shim invokes it, and nested procedures and rule-(3) extra entry
 names stay module-private as documented. Multi-entry primaries
 expose the primary thunk; the shared impl stays private. Each unit
 still compiles with `-c` and links with `cc` plus libpli; the
 driver keeps its single-input form.

 Consequences. `driver/multimod` links a MAIN unit against a
 library unit (function return plus `CALL` across the link);
 `tests/ir/func.check` now expects `@FACT`. Known limits: array and
 structure parameters need full entry descriptors (rule (38), M2);
 shared `EXTERNAL` variables need storage promotion from AUTOMATIC
 allocas to agreed globals and stay a follow-up; multi-file driver
 arguments stay a follow-up.

 ## ADR-104 — SELECT desugars to an IF-chain in the parser

 Context. `SELECT`/`WHEN`/`OTHERWISE` is the highest-value modern
 nicety (MX1) but has no TR production, so it enters through the
 Extensions vehicle with contextual keywords only: the lexer still
 classifies nothing, and `select` keeps working as an identifier.

 Decision. Desugar in `parseSelect` rather than adding an AST kind:
 a bare `SELECT` takes one boolean predicate per `WHEN`, while
 `SELECT (expr)` takes value lists compiled to `expr=value`
 comparisons ORed per clause (the expression is cloned per value).
 The result is ordinary `If` nodes, so sema, all statement walkers,
 ON-unit restrictions, HIR, and codegen work unchanged; `--print-hir`
 shows the chain. Missing `WHEN`, `WHEN`-after-`OTHERWISE`, a second
 `OTHERWISE`, mixed forms, and a labeled `END` for an outer block
 (multiple closure) are all handled; diagnostics cite ADR-104.

 Consequences. `select.pli` covers boolean WHENs, value lists,
 nested use, omitted OTHERWISE, and `select` as an identifier;
 `bad_select.pli` pins the missing-WHEN diagnostic. Known limits:
 `WHEN` value ranges/patterns beyond equality lists stay a
 follow-up; MX2–MX8 are untouched.

 ## ADR-105 — LEAVE/ITERATE over iterative DO-groups

 Context. Loop exit/continue (MX2) has no TR production, so it
 enters through the Extensions vehicle with contextual keywords
 only: `leave` and `iterate` keep working as identifiers. A dead
 `Stmt::Leave` kind (rejected in M0) already held the place.

 Decision. `LEAVE [label]` exits and `ITERATE [label]` continues
 the innermost enclosing iterative DO-group, or the named one;
 plain groups and `BEGIN` blocks are transparent, and a label
 naming one reports the same unknown-target diagnostic. WHILE
 re-entry branches to the condition, DO-loop re-entry to the
 step. Scope validation lives in sema on a loop-label stack;
 codegen keeps a parallel block stack. `LEAVE`/`ITERATE` inside
 an ON-unit is diagnosed alongside `RETURN` (the unit has no
 loop frame to branch to).

 Consequences. `leave.pli` covers unlabeled and labeled exit and
 continue over `DO`-loop and `WHILE` groups plus identifier
 coexistence; `bad_leave.pli` pins outside-loop uses and
 `bad_leave_label.pli` unknown/plain-group targets. Known limits:
 MX3–MX8 are untouched.

 ## ADR-106 — DO UNTIL as a post-test flag on the DO-group

 Context. Post-test loops (MX3) have no TR production; only `DO
 WHILE` (rules (69)–(73)) is served. The gap between them is one
 branch direction, so a new AST kind would duplicate every
 statement walker for no semantic difference.

 Decision. `DO UNTIL (expr)` sets an `until` flag on the ordinary
 DO-group (mirrored to HIR and shown by `--print-hir`); codegen
 emits body-first with a true-condition exit, sharing the
 LEAVE/ITERATE re-entry targets with WHILE. Iterative or combined
 WHILE+UNTIL forms are diagnosed, never silently accepted;
 `until` stays usable as an identifier via the existing
 assignment-lookahead dispatch.

 Consequences. `do_until.pli` covers runs-once, counting, nesting,
 and LEAVE/ITERATE interplay; `bad_do_until.pli` pins the
 iterative-combination diagnostic. Known limits: MX4–MX8 are
 untouched.

 ## ADR-107 — TRIM and TALLY string built-ins

 Context. Enterprise verification first: `TALLY(x, y)` is confirmed
 (FIXED BINARY(31,0) non-overlapping case-sensitive count, zero
 when absent or null, character and bit strings) and `TRIM` is
 confirmed as a both-ends trimmer; `LTRIM`/`RTRIM` do not appear in
 the Language Reference index, so they are not implemented rather
 than invented.

 Decision. `TRIM(s[, pad])` takes one or two character arguments
 and returns fixed `CHAR(n)` at the input length, left-justified
 and blank-padded (the TRANSLATE precedent; a null pad means
 blanks, an empty pad set trims nothing); varying-length results
 stay a follow-up. `TALLY(x, y)` takes two character arguments and
 returns `FIXED BIN(31)`. Non-character arguments and arities are
 diagnosed under rule (123) like the neighbouring built-ins; `BIT`
 stays diagnosed until longer bit strings arrive (QR2.2).

 Consequences. `trim.pli` covers blank and pad-set trims plus the
 all-blank input; `tally.pli` mirrors the IBM examples including
 the non-overlapping `aaa`/`aa` case; `bad_trim.pli` and
 `bad_tally.pli` pin the arity/type diagnostics. Known limits:
 MX5–MX8 are untouched.

 ## ADR-108 — VALUE named constants as read-only storage

 Context. Named constants (MX6) have no TR production. Folding
 use-sites to literals would break address-taken uses (`ADDR`,
 by-reference call arguments), so the stage keeps ordinary
 storage and enforces read-only use instead.

 Decision. `VALUE(const)` stores its folded constant through the
 existing scalar-`INITIAL` path (`initExpr`, initialized once at
 entry) and marks the symbol; every write position — plain and
 multiple assignment, `SUBSTR`-target, `GET LIST`/`GET EDIT`,
 `READ INTO`, `DO` control, `PUT STRING` (`GET STRING` reads and
 stays allowed) — is diagnosed via one helper. Scalar-only:
 arrays, structures, pointers, complex data, `FILE`, and
 `DEFINED`/`BASED` combinations are diagnosed, as is any
 `INITIAL`+`VALUE` combination (in either order). Diagnostics
 cite ADR-108.

 Consequences. `value.pli` covers fixed, float, character, and
 bit constants in expressions and a dynamic bound;
 `bad_value.pli` pins all seven write positions and
 `bad_value_init.pli` the combination (split because a parse
 diagnostic skips sema per the pipeline staging). Known limits:
 by-reference writes through call arguments are invisible to the
 checker; MX7–MX8 are untouched.
