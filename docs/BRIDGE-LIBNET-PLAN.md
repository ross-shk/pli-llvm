# Bridge plan: libnet toward TR-shaped PL/I

Goal: compile a ported libnet subset with plic-llvm without weakening
TR conformance. Two lanes: TR-shaped work (spec-conformant, normal ADRs)
and TR-silent work (marked extensions, ADR each). Anything TR-contradicting
stays on the libnet side as a restructure — no plic work.

## Lane A — TR-shaped (spec-conformant)

### A1. Programmer-named conditions (rules 94, 99)
Serve `CONDITION ( identifier )` in `ON`/`SIGNAL`/`REVERT`, reusing the
ADR-076 handler-id machinery with a per-condition stack (or a tagged
single stack). Declaration of the name follows the TR declaration rule
(spike: locate it — likely a `CONDITION` data attribute). No `ONCODE`
coupling: units read state (e.g. errno) through globals. Rejects today
under (99); tests: golden establish/raise/resume + `bad_` for undeclared
names. Unblocks libnet's `signal condition(neterror)`.

### A2. Minimal DISPLAY (rule 114)
Serve `DISPLAY ( scalar-expression );` (the C28/Y33 attested core) lowered
to a new `pli_display` runtime print. `REPLY` and the EVENT form stay
diagnosed for a later slice. Tests: golden output + `bad_` for non-scalar.
Unblocks libnet's `display` call sites.

### A3. Confirm silent items (spikes, no implementation)
Verify against TR text before anyone commits to work: `TRIM` and `LINKAGE`
have zero hits in the spec — if confirmed absent they move to lane B only
on explicit request, else libnet replaces them (`SUBSTR`-based trim;
plain `EXTERNAL` entries).

## Lane B — TR-silent (marked extensions)

### B1. Quoted %REPLACE re-scan (new ADR)
Extend the ADR-077 lexer pass: a `"..."` replacement operand is re-lexed
as source text before splicing. Unquoted form unchanged. Tests: golden
quoted form + the existing `replace.pli`/`bad_replace.pli` still green.

### B2. `%process` — no work
Stays diagnosed. Iron Spring-specific (`extended_replace`); the port drops
the line. Recorded here so it is not re-proposed.

### B3. Socket pattern — no compiler work
Calls already route through `ENTRY ... EXTERNAL` (ADR-021). Prove with a
tiny C stub + one ported client proc compiled, linked, and run in CI.

## Explicitly excluded (libnet-side restructures)
- `PACKAGE ... EXPORTS` → separate external procedures (rule 2 is closed).
- File-scope `dcl 1 ... STATIC` → in-procedure declarations.
- `SIGNAL ... SET ONCODE(...)` → no TR production; read an errno global
  in the unit instead.
- Single-byte `¬` sources → re-encode to UTF-8 or `^` (never repaired
  in-compiler, per repo rule).

## Order and acceptance
A1 → A2 → B1 → B3; A3 spikes anytime. Each slice follows repo workflow
(test first, AST → parse → sema → irgen → runtime, coverage + ADR, stage,
no commit). Acceptance: a ported subset (in-procedure `type_defs`, one
client proc, C stub) compiles, links, and runs a socket smoke test; full
`make test` stays green throughout.
