# Deferred work — the registry

One place for every item this compiler has deliberately not done yet. Before this
file, deferred work lived in three unlinked registries: the `D1`/`M3`–`M8`/`QR2.x`
labels in `GRAMMAR-COVERAGE.md`, the verdicts in `OPTIMIZATION-BASELINE.md`, and
prose inside the ADRs. Nothing crossed them, so an item could be deferred twice or
forgotten once.

This file is a **register**, not a specification and not a claim of completeness
for implemented features. `GRAMMAR-COVERAGE.md` remains the rule-by-rule truth
for what works; `DESIGN-DECISIONS.md` remains the reason things are the way they
are.

Out of scope here: release mechanics (tagging, packaging, versioning), the
benchmark corpus, and the release's platform matrix. One exception lives in
Band A: the gate that decides whether a release may ship at all (A5).

## The one rule

A row leaves this file in exactly one of three ways:

1. It is **served** — the feature lands, and its `GRAMMAR-COVERAGE.md` row moves.
2. It is **dropped** — recorded below with the reason and the ADR or document that
   settled it.
3. It is **recorded elsewhere** — as an extension in `GRAMMAR-COVERAGE.md`'s
   extensions table with its own ADR, never as if it were spec scope.

A row that just disappears is a bug in this file.

## Status vocabulary

| Status | Means |
|---|---|
| `open` | scheduled, nothing blocks it |
| `blocked` | needs another row of this file first (named in `Deps`) |
| `gated` | needs a measurement or a decision before it is worth building |
| `done` | served; row kept one release for traceability, then deleted |

`Size`: **S** ≤ 3 days, **M** 1–2 weeks, **L** 3+ weeks. These are relative
planning weights, not commitments — the repo has no velocity data to calibrate
them.

Short names: **GC** = `GRAMMAR-COVERAGE.md`, **OB** = `OPTIMIZATION-BASELINE.md`,
**DD** = `DESIGN-DECISIONS.md`, **PP** = `guides/programming.md`.

## Priority model

Four criteria, applied in order. The first one dominates every other
consideration.

1. **Silent wrong answer.** The compiler accepts a program and produces a value the
   language forbids. Worst class: the test suite pins the wrong answer, so the
   defect is invisible.
2. **Reach.** How many ordinary 1960s batch, business and scientific programs
   cannot be compiled or run without rewriting source.
3. **Leverage.** How much other work the item unblocks.
4. **Cost.** A cheap item that closes a deferral outright moves ahead of an
   equal-value expensive one.

Bands **A**–**D** apply that order; within a band, `Deps` decides the sequence.

## Band A — foundation and silent wrong answers

| ID | Work | Status | Evidence | Done when | Size | Deps |
|---|---|---|---|---|---|---|
| A1 | **Doc repair**: the stale references and the stale `PP` claims | done | The repair pass landed: `GC:11` no longer points at the deleted `QUICK-RELEASE-IMPLEMENTATION-PLAN.md`, `DD:5` and `OB:29` no longer point at a document the tree does not contain, `AGENTS.md:49` lists this file instead of the deleted `SPEC-COMPLIANCE-REPORT.md`, and `PP`'s "Not yet" bullets for division, label variables and the fiber backend were corrected | no reference to a file that is not in the tree; every `PP` "Not yet" bullet is true of the current tree; each open row below appears exactly once | S | — |
| A2 | **Declared-precision `SIZE`**: trap when a `FIXED BINARY(p)` value leaves its declared precision, not its storage width | done | Served by ADR-282: every conversion into `FIXED BINARY(p)` checks `[-2^p, 2^p-1]` and a resuming handler receives the value wrapped in `p+1` bits (`src/irgen.cpp`, `fixedBinPrecTrap`); the gate is `driver/precision_overflow` (all three conversion paths, the `bin(31)` edge, `--no-size-checks`); no existing test relied on storage-width wrapping, so the sweep was empty; `OB §11.1` re-measured | `driver/precision_overflow` traps at the declared precision and a `SIZE` handler resumes with the wrapped value; `(NOSIZE)` still elides; every test that silently relied on storage-width wrapping is updated with a cited reason; `OB §11.1` re-measured | M | — |
| A3 | **`FIXED BINARY(p,q)` scale semantics**: literal scaling, mixed-scale `+ - *`, `2^-q` on `FLOAT` conversion, list-directed output | done | Served by ADR-284: a scale-2 variable assigned `5` holds `20`; `+ -` and comparisons rescale to max q and `*` sums scales; `FLOAT` conversion is `sitofp` plus a multiply by 2^-q; list-directed I/O and DISPLAY print/read the point; `tests/core/scaled.pli` was rewritten as a cited conformance change (Open decision 1), with `bad_scaled_subscript.pli` as the negative. Picture output of a scaled binary is now ADR-288 and exact wide cross-base rescaling is ADR-289 | a scale-2 variable assigned `5` holds `20`, two scale-2 values multiply to the rescaled product, `FLOAT` conversion divides by 2^q, output prints the point; `tests/core/scaled.pli` is rewritten **as a conformance change with its own ADR**, not as a quiet update | M | decision 1 |
| A4 | **`BIT` padding model** written down, closing `C8`'s residual | done | Served by ADR-283: the padding bits are indeterminate and are not part of the value, every value operation ignores them, and `NOT` complements the declared bits and normalises the padding to zero; `tests/ir/bit_padding.pli` pins the `xor`/`and` lowering, ADR-243/ADR-275 are cross-referenced, and `OB §9.6` now points at the ADR | the model is stated once (padding bits are indeterminate and unobservable except through `NOT`), `tests/ir/bit_padding.pli` pins `NOT` over a padded value, and ADR-243/ADR-275 are cross-referenced | S | — |
| A5 | **Release pipeline gate**: the release workflow's `Test` step is `continue-on-error: true` with nothing after it that consumes the result, so a tag can publish a red suite | done | Served: `release.yml`'s `Test` step is a hard gate again (`.github/workflows/release.yml:105`), so a red suite fails the build job and `publish` (`needs: build`) never runs; the soft step entered in `9ba489a` when the suite had pre-existing failures. Proof: a planted `zz_gate_probe.pli` drove the job's `ctest` command to exit 8, and the probe was removed | the release workflow fails the build job on a red suite, or records an explicit, reviewed reason why it does not; a deliberately failing test proves the gate fires | S | — |
| A6 | **`DCL x(n) FILE`/`ENTRY`/`LIKE` parse as scalars**: the dimension lookahead in `tryParseDimension` knew only some attribute words, so a bare `(n)` before `FILE`, `ENTRY` or `LIKE` became a precision the item cannot use and the item ended up scalar | done | Served by ADR-337: the lookahead gained `FILE`, `ENTRY`, `LIKE`, `COMPLEX`, `CPLX` and `REAL`. Arrays of `COMPLEX` now work end to end (`complex_array.pli`), including the subscript- and member-load defect that made `.cpx` null and segfaulted the compiler on `z = c(2) + c(3)` and `real(s.m)`. Arrays of `FILE` and `ENTRY` are diagnosed with rule (12) in both spellings — a file array needs one runtime slot per element and a subscripted file designator, an entry array a subscripted call designator — instead of collapsing to one slot (`bad_file_array.pli`, `bad_entry_array.pli`); `LIKE` with a dimension is served as an array of the template (`like_array.pli`, ADR-338), which also stopped the template's own dimension from being inherited silently | each of the three either declares the array (and the operations on it work) or is diagnosed with rule (12) instead of silently collapsing to a scalar | S | ADR-336 |

**Why A2 before A3.** Both are silent-wrong-answer items. A2 is first because
three separate documents name the same gap and because it gates D1; A3 is second
because it also rewrites a green test, which needs an explicit decision.

## Band B — reach: what real programs still cannot do

Ordered by the size of the program class each unblocks, not by the size of the work.

**Preprocessor work is parked.** B6 is the lowest-priority row in this plan
and lives in the Parked section after Band D: take it up only after every
non-preprocessor row in Bands B-D is served or recorded. Its size and reach
are not in question; the near-term sequence stays on the language and runtime.

| ID | Work | Status | Evidence | Done when | Size | Deps |
|---|---|---|---|---|---|---|
| B0 | **Sanitizer matrix**: ASan/UBSan for the storage and overlay paths, a race check for tasking, and explicit (not silent) skips | done | Served: `tests/run_tests.py` counts skips in the summary and prints a per-group sanitizer table; `tests/driver/storage_asan.sh` builds the runtime from source with ASan+UBSan and runs twelve self-checking storage, overlay and varying-string programs (`based_*`, `controlled_*`, `defined*`, `varying_nested`); `tests/driver/tasking_tsan.sh` builds it with TSan and runs `task`/`task_many` under the fiber and OS-thread models; `lifetime_asan.sh` marks its ASan run too. Each test prints a `SANITIZER:` marker, and a missing runtime or a non-Unix host is a counted skip. The emitted program object stays uninstrumented in these runs — B13/ADR-341 adds the driver option for it | each test group states which sanitizer ran; skips are counted and visible in the run; tasking has a race-detection run | S | — |
| B1 | **`PICTURE` family (`D1`)**: `DECLARE ... PICTURE`, picture variables, `GET EDIT P`, sterling data, wide picture items | done | Served by ADR-290/291/292 for numeric pictures (`picture_var.pli`, `picture_input.pli`, `picture_list.pli`), ADR-293 for wide pictures (`picture_wide.pli`, `bad_picture_wide.pli`), ADR-294 for sterling constants (`sterling.pli`, `bad_sterling.pli`) and ADR-295 for `G`/`M` sterling pictures (`sterling_picture.pli`, `money_report.pli`, `bad_sterling_picture.pli`). The supported sterling subset is `9 Z . B / H P`; fractional pence, sign editing and the single-character forms stay diagnosed. | a picture-declared variable round-trips through `PUT EDIT`/`GET EDIT` with runtime format interpretation; sterling constants and their pictures work; `bad_picture*.pli` negatives cite their rules; report and money goldens exist | L | runtime formatter (see Dependency map) |
| B2 | **Sequential record update (`M6a`)**: `REWRITE`, `DELETE`, `ON ENDFILE` | done | Served by ADR-285/286: `ON ENDFILE(f)` is a per-file condition (`bad_endfile.pli`), `OPEN ... RECORD SEQUENTIAL UPDATE` opens `r+b`, `REWRITE`/`DELETE` act on the current record, and a short READ resumes through the ENDFILE unit or raises ERROR unhandled; the gate is `tests/core/record_update.pli`, a six-record master-file update run under ADR-285's loop and under ASan | a master-file update program (read loop, `ON ENDFILE`, conditional `REWRITE`/`DELETE`) runs against a golden | M | — |
| B3 | **Format and display remainder (`M5`)**: `COPY`/`LINE`, dynamic format counts, `GET` `COL` positioning, `DISPLAY REPLY` | done | `GC` (105)–(111),(114) | one golden per item; a dynamic-count negative; report programs stop needing source rewrites. Dynamic counts stay diagnosed with rule (48) (`bad_edit_iter.pli`); the rest are served (`line_put.pli`, `driver/stream_opts`, `get_col.pli`, `driver/display_reply`, ADR-287) | M | — |
| B4 | **Conditions remainder (`M4`)**: the remaining condition kinds, ON-unit `RETURN(value)`/`GO TO`, non-local `GO TO` | open | Served in part by ADR-297: STRINGRANGE is IBM's opt-in condition, enabled by a `(STRINGRANGE)` prefix (statement-, group- or PROCEDURE-scoped) and raised by an out-of-range `SUBSTR` argument triple, with the clamped resume value and the notify-and-continue implicit action (`on_stringrange.pli`, `stringrange_implicit.pli`); `bad_on_cond_stringrange.pli` pins the `CONDITION()` exclusion. ADR-299 serves `DECLARE` in a `BEGIN` ON-unit: the unit is a block, its locals shadow outer names, persist across invocations, and ride the existing capture path (`on_unit_decl.pli`, `bad_on_unit_decl.pli`). ADR-309 serves `UNDEFINEDFILE(f)`, the second per-file condition after ENDFILE: a failing stream or RECORD `OPEN` raises it against the file's slot, a handler resumes after the `OPEN` with the file left closed, an unhandled or reverted condition takes ERROR's abort, and `PUT`/`GET FILE(f)` on the closed file raise ERROR instead of silently using SYSPRINT/SYSIN (`undefinedfile.pli`, `driver/undefinedfile`, `bad_undefinedfile.pli`, `bad_undefinedfile_nofile.pli`). ADR-320 served the `CHECK`/`NOCHECK` prefix elements (rules (61),(95),(96)): `CHECK` clears a disable inherited from an enclosing `NO...` prefix, `NOCHECK` alone disables every implemented check, `CHECK(ALL)` turns on the opt-in STRINGRANGE check, and either form on a `PROCEDURE` statement scopes its body including nested procedures (`check_prefix.pli`); the prefix list on a `PROCEDURE` statement used to drop its `NO...` flags silently, which ADR-320 also fixes. ADR-321 served subscripted and qualified access from a BEGIN ON-unit to fixed-size automatics of the establishing frame, captured by base address, with BASED/CONTROLLED/variable-extent storage and nested units still diagnosed (`on_unit_aggregate.pli`, `bad_on_local.pli`). ADR-322 served `FINISH`: the per-task key is raised when the procedure the task was created for exits, before its event completes and while the unit is still established (`on_finish.pli`, `bad_on_cond_finish.pli`). ADR-323 served `AREA`: the region runtime raises it for an ALLOCATE that does not fit and for a pointer that does not belong to the region, and a resuming unit leaves the operation undone (`on_area.pli`, `driver/area_condition`, `bad_on_cond_area.pli`). ADR-326 served `UNDERFLOW` for floating-point operator results: a subnormal result, or a `*`/`/`/`**` result flushed to exact zero, becomes 0 and raises the condition; `(NOUNDERFLOW)` and CHECK cover it and the notify-and-continue implicit action is pinned (`on_underflow.pli`, `bad_on_cond_underflow.pli`). ADR-333 served `OVERFLOW`: the floating-point operators raise it when a result exceeds the largest double, the value stays the IEEE infinity, a unit that returns normally raises ERROR, and the implicit action is a message followed by ERROR (`on_overflow.pli`, `driver/overflow_conditions`, `bad_on_cond_overflow.pli`). ADR-334 served `FIXEDOVERFLOW`: a `FIXED DECIMAL` `+`, `-` or `*` whose result needs more than the 31 digits PL/I allows raises it, a returning unit resumes, `FIXED BINARY` never raises it, and the implicit action is a message followed by ERROR (`on_fixedoverflow.pli`, `driver/overflow_conditions`, `bad_on_cond_fixedoverflow.pli`); a decimal intermediate that wraps its 64-bit storage still takes the SIZE path, which is B11's exact-decimal work. ADR-336 served the initial-label construct the row carried (rule (65)): an element of a fixed-extent label array names a statement as a prefix and holds that statement's address, a label constant or another label variable can be assigned to a label variable or an element, and the indirect `GO TO (expr)` / `GO TO variable` jump through them (`label_computed.pli`, `bad_label_elem.pli`, `bad_label_elem_bounds.pli`, `bad_goto_operand.pli`). Still open: the keyed-I/O `KEY`/`RECORD`/`TRANSMIT`/`NAME`/`ENDPAGE` conditions (B7), ON-unit `RETURN(value)`/`GO TO`, and non-local `GO TO`; the `SUBSTR` pseudo-variable stays unchecked because IBM defines its out-of-range store as an invalid program. `GC` (60)–(63),(65),(77),(91)–(99); `PP`'s conditions section | a matrix test per condition covering establish, raise, resume, revert, default action; `GO TO` out of an internal procedure; initial-label behaviour pinned after the damaged-scan question is settled | L | — |
| B5 | **`AREA`/`OFFSET` completion (`M3`)**: STATIC/EXTERNAL areas, arrays of OFFSET, offset arithmetic | done | ADR-300 served locator member arrays: `P -> S.A(i)` reads and writes through the locator value, not the structure's declared BASED pointer, for static member arrays including nested minor-structure members and CHAR elements (`locator_member_array.pli`); `bad_locator_member_array.pli` pins the non-BASED diagnostic, and non-locators, dynamic member arrays, cross-sections and locator array-of-structures members stay diagnosed. ADR-301 served the `FREE` locator: the reference before `->` is any locator-valued reference (POINTER or OFFSET), so a subscripted element, a qualified member or a nested chain frees the block it addresses, the parenthesised item list frees several, and `IN ( AREA )` rides the locator (`free_locator_expr.pli`, `bad_free_operand.pli`). ADR-303 served `AREA(*)`: on a procedure parameter the size is inherited from the argument, so the callee allocates in the caller's region (`area_star_param.pli`, `bad_area_star.pli`), an `ENTRY` descriptor accepts it as interface shape, and a non-parameter declaration is diagnosed; a STATIC AREA was diagnosed with rule (20) instead of being silently automatic (ADR-310 serves it). ADR-304 served arrays of AREA: each element gets its own region created and destroyed with the block, `IN ( ar(i) )` names one region in ALLOCATE and FREE, an element or the whole array passes to an `AREA(*)` parameter (`area_array.pli`), and a whole array or a runtime extent in `IN` is diagnosed (`bad_area_array.pli`). ADR-310 served STATIC areas: the region lives for the whole program, created on first use through `pli_area_lazy`, so a block allocated by one activation stays readable and FREE-able in the next (`area_static.pli`); an array of static AREA keeps one region per element, a whole array argument materialises its elements before the call, a BEGIN-block static area outlives the block, and a package-level AREA shares one region (`area_static_package.pli`); EXTERNAL areas and STATIC on a parameter stay diagnosed (`bad_area_static.pli`). ADR-311 served offset arithmetic: `OFFSETADD`/`OFFSETSUBTRACT` move an offset by a numeric byte count and `OFFSETDIFF` returns the FIXED BIN(31,0) byte distance between two offsets (`offset_arith.pli`, `bad_offset_arith.pli`), while the `OFFSET`/`OFFSETVALUE` conversion built-ins stay open because the lowering keeps no area base. ADR-312 served arrays of OFFSET: an element is a SET target, a `->`/FREE locator and a scalar offset-arithmetic operand (`offset_array.pli`), while a whole array is still rejected (`bad_offset_array.pli`). ADR-317 corrected the SET rule: the AREA named in an OFFSET declaration is the default region, so `allocate x`/`allocate x set(o)` and `free x`/`free o -> x` without `IN` use it, and an explicit `IN` may name a different area (the association is not permanent); an unassociated OFFSET without `IN` stays diagnosed (88)/(90), and an AREA structure member is diagnosed with rule (20) (`area_implicit.pli`, `bad_area_offset.pli`). ADR-318 served EXTERNAL areas: the region rides rule (42)'s common global slot and is created on first use, so every module reaches one region (`multimodule/area_external.pli`, `bad_area_static.pli`). ADR-319 served the `OFFSET(pointer, area)` conversion, validated against the region (`offset_convert.pli`, `bad_offset_convert.pli`); `OFFSETVALUE` stays diagnosed because an offset holds an absolute address here, and that is the recorded limitation. `GC` (20),(22),(23),(88),(90),(124); `PP`'s area section | the alloc/free order matrix passes under ASan; locator member arrays read and write on both sides; unsupported combinations keep cited diagnostics | L | B0 |
| B7 | **Keyed/direct record I/O (`M6b`)**: keyed/update/direct organisations, `LOCATE`/`UNLOCK`/`NOLOCK`/`SET`/`EVENT`, `KEY`/`KEYTO`/`KEYFROM`, and the file attributes `IDENT`/`LINESIZE`/`PAGESIZE`/`ENVIRONMENT` | blocked | `GC` (100)–(105),(112),(113) | round trips per organisation against a documented host file format; needs its own ADR — IBM keyed files are not portable (`Open decisions` 3) | L | B2, decision 3 |
| B8 | **Descriptor and array-form remainder (`QR2.1`)**: dope-vector descriptors, `POSITION`, subscripted `DEFINED` bases, per-`ALLOCATE` dimension bounds, multiple-`*` descriptor dimensions, parameter correspondence | open | Served in part by ADR-296: a 1-D BASED array takes a per-`ALLOCATE` extent (`DCL a(*) BASED(p); ALLOCATE a(n) SET(p)`), which sizes the block and drives subscripts, `LBOUND`/`HBOUND`/`DIM` and re-allocation (`based_alloc_dim.pli`), while multi-axis, `lb:ub` and `ALLOCATE (*)` stay diagnosed (`bad_based_alloc_dim.pli`). ADR-305 served multi-axis iSUB: a `kSUB` dummy names the item's k-th subscript, so the axes may cross (a transposed overlay) and one dummy may drive several base axes, with affine arithmetic per axis (`isub_multi.pli`, `bad_isub_defined.pli`). ADR-306 served rule (24) `POSITION(n)`: a CHARACTER view starting at the n-th character of the base or of one base element, with the 1-based position and the fit checked at compile time and writes reaching the base (`defined_pos.pli`, `bad_defined_pos.pli`); a BIT or numeric base, an array item and `POSITION` with `iSUB` stay diagnosed. ADR-307 served a runtime `DEFINED` base subscript: an expression is re-evaluated at every reference to the item, the alias follows its index, and a slip is bounds-checked against the base axis (`defined_runtime.pli`, `defined_runtime_range.pli`, `bad_defined_runtime.pli`); a base with a runtime-extent axis is now diagnosed with rule (126) instead of addressing past its declared placeholder extent. ADR-339 served rule (24) array overlay defining: a whole-array base is viewed by a shorter or other-rank DEFINED array as long as it fits and the element type (or storage size) matches (`defined_array.pli`, `bad_defined_array.pli`), while a runtime-extent side stays diagnosed. Still open: `GC` (34)–(38) tail ("full dope-vector descriptors → M2", multiple-`*` dimensions diagnosed) | generated shape and alias matrix passes; each item either works or keeps its cited diagnostic with a recorded reason | L | — |
| B9 | **Entry and interop surface (`QR2.6`)**: mixed-return alternate entries, C structure returns, `SECONDARY`/`CELL`, `ABNORMAL`/`NORMAL`, `REDUCIBLE`/`IRREDUCIBLE` | open | ADR-302 served `USES`/`SETS`: an `ENTRY` declaration accepts either attribute in any order, checks each item (an unsubscripted name/qualified name, an integer level, or `*`), and records the lists on the entry symbol as interface metadata with no codegen effect; a storage item carrying them cites rule (34) and a malformed item rule (38) (`entry_uses_sets.pli`, `bad_uses_sets.pli`, `bad_uses_sets_item.pli`). `GC` (38): mixed returns and C structure returns diagnosed. ADR-308 served `ABNORMAL`/`NORMAL`: `DCL e ENTRY ABNORMAL(label)` sends a `CALL` through `e` to the label instead of the statement after the call, reverting the ON units of any `BEGIN` block it leaves, and `NORMAL` names the ordinary return point; the label must be a top-level label of the declaring procedure, and a non-entry, a function entry, both attributes together, an `ENTRY VARIABLE`, an asynchronous call, a missing or nested label, and a call from another procedure are diagnosed (`abnormal_entry.pli`, `bad_abnormal_entry.pli`). ADR-335 served `REDUCIBLE`/`IRREDUCIBLE`: both entry-name attributes (rule (34)) are accepted on an `ENTRY` declaration in any order and recorded on the symbol as a reducibility hint, a storage item carrying one cites rule (34), and nothing lowers them yet because no interprocedural value numbering exists (`entry_reducible.pli`, `bad_reducible.pli`). Still open: mixed-return alternate entries, C structure returns, `SECONDARY`/`CELL` | each item either lands with a test or is recorded as an accepted extension or limitation in `GC` | L | — |
| B10 | **Source compatibility (`M8`)**: `GENERIC`, the remaining keyword abbreviations, 48-character-set forms | gated | `GC` (41) `GENERIC` = M8, aux §2.3.2.1 and §2.3.3; GC's own note says "prioritize by corpus impact" | a corpus ranking exists first (`Open decisions` 4), then a dispatch and signature matrix plus an abbreviation golden | M | decision 4 |
| B11 | **Numeric data remainder (`D1`/`QR2.2`)**: the `IMAGINARY` data and I/O cases `GC` routes to `D1`, complex I/O completeness (`EDIT`/`CHAR`/`FIXED` of a complex value), and the tail of full decimal and precision conformance | open | `GC` (16),(17) "full decimal/precision conformance → D1/QR2", (128),(129) "imaginary/sterling → D1", (135)–(139) "decimal/complex I/O completeness"; `CHAR` and `FIXED` of a complex value are diagnosed today (`guides/builtins.md`); wide cross-base conversions are served by ADR-289 (`wide_cross_base.pli`); `P` output of a scaled binary is ADR-288 (`scaled_picture.pli`) | each item lands with a test or is recorded as an accepted limitation; the diagnostic set for complex is exhausted | M | — |
| B12 | **Complex exponentiation (`**`, rule 121)** | done | Served by ADR-182: both the integer-exponent and the complex-exponent forms go through `exp(b*log(a))` over the pair layout, and `tests/builtins/complex_pow.pli` gates `z ** 2` and `z1 ** z2` within tolerance; `GC` (115)–(122) already cites the test, so this row was stale | `z ** n` for integer `n` and `z1 ** z2` produce correct results or keep a cited diagnostic; a small reach item, so it sits last in this band | S | — |
| B13 | **Sanitizer instrumentation for emitted code**: the B0 runs instrument the runtime built from source, not the code `irgen` emits | done | Served by ADR-341: `-fsanitize=address` marks the emitted functions `sanitize_address`, opts the embedded runtime/platform blobs out (B0 covers them, and the fiber stacks break ASan's stack model), runs LLVM's `AddressSanitizerPass` before optimization, and links compiler-rt's runtime through in-process lld (Darwin dynamic + `-rpath`; ELF static). `tests/driver/emitted_asan.sh` reports a one-past stack array and a one-past BASED allocation (`stack-buffer-overflow`/`heap-buffer-overflow`, non-zero exit), keeps an in-bounds run clean, proves the same overflow is silent without the option, compiles `-c`, and pins the refusals (`-fsanitize=thread`, `-fsanitize=address --flto`); a toolchain without compiler-rt is a counted SKIP with a `SANITIZER: address` marker. `undefined`/`thread` stay diagnosed | either a `-fsanitize=address\|undefined\|thread` driver option adds the LLVM instrumentation and links the matching runtime, or the measured scope of the B0 runs is recorded as the accepted limitation | M | B0 |

## Band C — runtime and platform completeness

| ID | Work | Status | Evidence | Done when | Size | Deps |
|---|---|---|---|---|---|---|
| C1 | **Fiber backend proof**: a tasking matrix per backend (switch, wake, park, overflow) and the doc note kept true | done | `tests/driver/task_matrix.sh` compiles `tests/core/task_matrix.pli` and runs it on fiber/1, fiber/4, os/1, os/4 across x86-64 Linux, arm64 macOS, and Windows CI; overflow diagnostics proven via `tests/driver/task_stack_overflow.sh` | the matrix runs on x86-64 Linux, arm64 macOS/Linux and Windows; overflow diagnostics proven on all three | M | — |
| C2 | **Tasking remainder (`QR2.8`)**: the `PRIORITY` built-in, external async `CALL TASK`, stream file-read parking | done | `PRIORITY` reads/sets via `pli_task_priority_of`/`pli_task_set_priority_of` with relative spawn and live updates (ADR-298, `priority_basic.pli`, `priority_nested.pli`); external entry `CALL TASK` in `tests/driver/ext_async_test.sh`; stream `GET` parks through `rt_fbroker.c`; `SAN_RT_SRCS` includes `rt_fbroker` so the TSan/ASan drivers link | `PRIORITY` reads and sets a task's priority; external entry `CALL TASK` runs; a stream file read parks the task; a race-detection run is clean | M | C1 |
| C3 | **Inline-vs-library decision layer (`K2`+`K4`)**: a per-call-site predicate per supported built-in, plus the library-call remark | open | `OB §12.2` measured the library path **22.55x** slower; ADR-254 landed the mechanism and one instance (ADR-241) | each supported built-in has a predicate seeded from IBM's tables and a remark; a paired benchmark exists where the chosen path flips | L | — |
| C4 | **Record-file `READ` parking**: route the record read paths through a broker | done | Served by ADR-342: `pli_record_read_{fixed,float,char,bit}` hand the blocking read to `rt_fbroker_fread`, so the task parks under the fiber model; a fiber-aware `rt_rec_read_mu` makes the `ftell` -> read -> position update atomic, and the broker serialises its one work slot so a record `READ` and a stream `GET` cannot overwrite each other's request. `tests/driver/task_record.sh` starts a reader on a FIFO and a sleeping ticker on one worker: the mark only appears when the read parks, then the driver sends the record and checks the value; the test needs a POSIX FIFO and is skipped on Windows. `WRITE`/`REWRITE`/`DELETE` stay on the direct blocking path | a record `READ` parks the task under the fiber model | S | C2 |
| C5 | **Multi-worker fan-out could lose the shutdown wakeup**: a 200-task fan-out repeated 20 times intermittently left every worker parked after MAIN had finished | done | Root cause: MAIN can be stolen while it is moved from its home worker's `ready` list into the run queue, so it may finish on a peer. The peer sets `rt_main_done` and wakes all workers, but `rt_worker_park`'s re-check did not look at `rt_main_done`; a wake that arrived just before worker 0 marked itself parked was skipped, and nothing signalled again. The diagnostic build showed `rt_main_done=1`, `main state=DEAD`, all queues empty, all tasks finished. `rt_worker_park` now re-reads the flag in the spin and under the lock (ADR-332). Evidence: 0/40 timeouts at 8 workers, 0/10 at 2/4/6/8; `tests/core/task_pool_stress.pli` is the regression (20 rounds x 200) | a 20-round fan-out exits under fiber/8, and `tests/core/task_pool_stress.pli` stays green | M | C1 |

## Band D — measurement-gated performance

Every row here is worth building **only** with its measurement in hand. Each gets a
decision date when it is taken up; a missing measurement closes the row with the
measurement recorded, as `OB` already does for `I1`, `K5` and `K10`.

| ID | Work | Status | Gate to promote | Size | Deps |
|---|---|---|---|---|---|
| D1 | Range facts for declared precision, then re-measure the checks-on ratio against 1.5x | open | A2 landed (ADR-282); `OB §11.1`, ADR-252. The win is now only for a `p` narrower than its slot: at `p = 31`/`63` the storage check is the precision check, and `stampFixedBinRange` also needs the ADR-282 range before a fact is emitted | M | — |
| D2 | `C5` exponent patch: `sitofp` followed by a multiply by 2^-q | done | Served by ADR-284 in the A3 change: `convert()` lowers `FIXED BINARY -> FLOAT` as `sitofp` then `fmul 2^-q` (factor from `std::ldexp(1.0, -q)`), not a divide; ADR-244 is released (`OB §9.4`) | S | A3 |
| D3 | `E5` narrow-type codegen: add a 32-bit target benchmark first | gated | a narrow-integer workload on a 32-bit or embedded target shows a gap; on x86-64 the expected gain is ~0 (`OB §10.3`) | S–M | measurement |
| D4 | `I2` loop fusion: add the workload first | gated | a measured multi-pass array pattern exists; LLVM's loop-fusion pass is not in the default pipeline (`OB §11.2`) | M | measurement |
| D5 | Extension track: `RESEXP` and `INLIST`/`INARRAY` | gated | own ADR each plus an `Extensions (not in TR 25.084)` row; rejected as optimizations because they change which conditions a program raises (`OB §12.6`, §12.8); ADR-259 is reserved for `RESEXP` | M | ADR accepted |
| D6 | P2 architectural items 11 and 12: SSA promotion in `IRGen`, `llvm.assume` overflow-narrowing hints | gated | close unless a benchmark regresses; parity targets are already met (`benchmarks/OPTIMIZATION-PLAN.md:7-10`) | S | measurement |

## Parked — deliberately last

These rows stay in the register but are worked only after every open row in
Bands B-D is served or recorded. They are not blocked by a dependency; they
are a deliberate ordering decision.

| ID | Work | Status | Evidence | Done when | Size | Deps |
|---|---|---|---|---|---|---|
| B6 | **Preprocessor `QR2.9`**: `%DO`, `%GO TO`, compile-time procedures, replacement and include rescanning | done | Served by ADR-316 (Stages 1+2) and ADR-327 (Stage 3): `%DO`/`%END` iteration, `%GOTO`/`%GO TO`, and `%PROCEDURE`/`%PROC`/`%RETURN` compile-time macros run in the textual preprocessor (`pp_do.pli`, `pp_goto.pli`, `pp_proc.pli`, `bad_pp_do_unterminated.pli`); `%REPLACE name BY <text>;` is recorded by `handleReplace` and `scan()` expands the name as a whole identifier word, rescanning the replacement text so it may name another replacement, with a re-entry guard that terminates self and mutual cycles. A quoted `"..."` operand keeps its ADR-082 rescanning (stripped and rescanned at each use), a stray `"` is diagnosed, and the lexer's token-level pass is gone. `replace.pli`/`replace_quoted.pli` stay byte-identical; `replace_rescan.pli` covers chaining, quoted rescan, self-reference and case-insensitive names, and `replace_include.pli` covers the map in both include directions; `bad_replace_name/by/empty/mixed.pli` and `bad_dqstring.pli` pin the malformed forms | `%REPLACE` textual rescanning moved from lexer to preprocessor (Stage 3, ADR-077→ADR-327) | M | — |

## Dropped, or deliberately out of scope

Do not re-open these without a new record.

| Item | Why | Record |
|---|---|---|
| PGO+LTO combined | needs link-time PGO | ADR-225 |
| MLIR as the default or only level | dependency and risk outweigh a measured need; only a narrow helper bridge is admitted | ADR-158, ADR-230/231 |
| A user-facing `ORDER`/`REORDER` attribute | the capture-context mechanism gives the guarantee without it | ADR-253 |
| The original "BCD path" for wide decimals | replaced by the i128 scaled-integer model | ADR-191 |
| Fuzzing `decimal_ops` to match C `double` | C is inexact by design; PL/I's exactness is the feature | `benchmarks/DECIMAL-PERF-PLAN.md §8` |
| `benchmarks/opt-refinement-plan.md`'s `size_storm.pli` and the CI trend chart | deferred to CI infrastructure, not compiler work | `benchmarks/opt-refinement-plan.md:475`, `benchmarks/OPTIMIZATION-PLAN.md:249` |

## Dependency map

- `A1` (landed) was the tracking surface every other row depends on; its standing
  duty is now a working rule.
- `A2` → `D1` (the precision check must land before range facts are emitted).
- `A3` → `D2` (served together by ADR-284: the language feature and its
  exponent patch).
- `B0` → `B5` (a sanitizer run before the area and locator work ships).
- `B2` → `B7` (sequential update before keyed organisations).
- `C1` → `C2` (a tasking matrix before more tasking semantics).
- **Cross-stream joints all land in the runtime**: `B1` (picture formatter),
  `B5`/`B7` (region and file layers), `A2`/`A3` (numeric runtime entry points).

## Working rules

- Tests are the source of truth: a row is done when its gate test passes and is in
  the suite, not when the code compiles.
- One atomic commit per task, message naming the row ID.
- New `.pli` tests follow the card margins: one leading space, text starting in
  column 2, nothing past column 72.
- Run the smoke group plus the new test per change; the full suite only at band
  boundaries, because it is expensive.
- ADRs are append-only. Claim a number **before** writing: 259 is reserved for
  `RESEXP` and 343 is the next free number; 244 was released when ADR-284 served
  `C5`. Collisions happen: 192/194 and 239 were renumbered (ADR-232, ADR-279),
  the fiber-stack entry that repeated the ABNORMAL/NORMAL ADR-308 was
  renumbered to ADR-313, and the ASan ADR is now 341 after the GET branch took
  ADR-340. ADR-342 served C4 (record `READ` parking).
- A row that changes user-visible behaviour without an ADR is a documentation bug.
- Keep this file true: a new deferral gets a row in the same commit that creates
  it, and a served row moves its `GRAMMAR-COVERAGE.md` entry in the same commit.

## Open decisions

1. **A3 rewrites a green test — settled.** `tests/core/scaled.pli` pinned the
   unscaled model. The repo rule forbids rolling back conforming features, so
   the rewrite landed as a cited conformance change under ADR-284. The negative
   gate is `bad_scaled_subscript.pli`; picture output is served by ADR-288.
2. **A2 blast radius — settled.** Declared precision is enforced by default
   under the `SIZE` checks; the policy is to update an expectation with a cited
   reason rather than hold the conformance fix behind a flag (ADR-282). The
   sweep found no existing test that relied on storage-width wrapping.
3. **B7 needs a file-format decision.** What is a keyed file on a POSIX host — an
   index sidecar, a simple hashed layout, or IBM-compatible blocks? Nothing should
   be implemented before this is written down.
4. **B10 needs a corpus ranking that does not exist.** The archived plan assumed a
   "corpus report" of the most frequent remaining blockers. Either rank by hand or
   budget a small script that compiles a program set and tallies the diagnostics by
   rule number.
