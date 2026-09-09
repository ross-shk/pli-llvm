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


