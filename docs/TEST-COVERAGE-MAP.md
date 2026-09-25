# Test Coverage Map — PL/I Compiler (plic)

Generated: 2026-09-25 | Compiler plic (PL/I → LLVM)

## Test Inventory Summary

| Directory | Files | Type | Purpose |
|-----------|-------|------|---------|
| `tests/core/` | ~130 golden tests + ~95 bad_*.plI | Golden (`expected/*.out` diff) | Core language grammar, semantics, IR generation |
| `tests/builtins/` | ~60 | Self-checking + golden | Built-in function tests (math, string, array, pointer, complex) |
| `tests/usecases/` | 6 | Self-checking (print PASS/FAIL) | End-to-end integration programs |
| `tests/driver/` | ~19 | Shell scripts | Integration/infrastructure testing |
| `tests/ir/` | 9 | LLVM IR inspection (`grep` on `-emit-llvm`) | IR correctness validation |
| **Total** | **~319** | — | — |

---

## Rule-by-Rule Test Mapping

### Program Structure (Rules 1–8)

| Rule | Feature | Core Tests | Driver Tests | Verified? |
|------|---------|------------|--------------|-----------|
| (1) | program structure | hello.pli (+all), multientry.pli, procs.pli | driver/multimod, driver/package | Yes — M0 |
| (2) | procedure declaration | hello.pli, procs.pli | — | Yes |
| (3) | multi-name entry | multientry.pli | — | Yes |
| (4) | parameter lists | procs.pli, func.pli | — | Yes |
| (5) | options/recursive/returns | func.pli, recursive.pli, bad_recursive.pli, bad_recursive_mutual.pli, options_ignore.pli | — | Yes |
| (6),(7) | sentencelist/end-clause/closure | loops.pli, begin.pli | — | Yes |
| (8) | statement kinds | (internal procedures reach AUTO via static link) | staticlink.pli | Yes |

### Declarations & Attributes (Rules 9–43)

| Rule | Feature | Core Tests | Bad Tests | Verified? |
|------|---------|------------|-----------|-----------|
| (9),(10) | DECLARE | ifelse.pli | — | Yes |
| (11) | level numbers/factoring | struct.pli, struct_array.pli, factor.pli, struct_charmem.pli, struct_array_of.pli | bad_attr_range.pli | Yes |
| (12),(13) | dimensions/bounds | array.pli, array2d.pli, dynamic_array.pli, dyn_lower.pli, dyn_multi.pli, dyn_param.pli, star_param.pli, dyn_param_lower.pli | bad_array_oob.pli, bad_array2d_oob.pli, bad_array2d_arity.pli, bad_dyn_lower.pli, bad_dyn_multi.pli | Yes |
| (14),(15) | data attributes | pointer.pli, complex_var.pli, bitn.pli | bad_pointer.pli, bad_complex_var.pli | Yes |
| (16),(17) | arithmetic precision | decimal.pli, scaled.pli, decimal_io.pli | — | Yes |
| (18) | string attributes | bitn.pli, starchar.pli | bad_bitn.pli, bad_bitn_array.pli, bad_starchar.pli, bad_starchar_varying.pli | Yes |
| (23) | storage classes | controlled.pli, implicit_controlled.pli | bad_controlled_alloc.pli | Partial |
| (24) | DEFINED | defined.pli, isub_defined.pli, isub_arith.pli | bad_defined.pli, bad_isub_defined.pli | Yes |
| (25) | BASED | based.pli, based_param.pli, based_return.pli | bad_based.pli | Yes |
| (26)–(32) | INITIAL | init_array.pli, struct_init.pli, init_call.pli, dyn_init.pli, struct_dyn_init.pli | bad_init_array.pli, bad_struct_init.pli, bad_init_call.pli | Yes |
| (34)–(38) | ENTRY/RETURNS/LINKAGE | entry_mixed.pli, cbyvalue.pli, char_func.pli, starchar_entry.pli, entry.pli | bad_cbyvalue.pli, bad_returns_attr.pli, bad_char_func_star.pli | Partial |
| (39),(40) | FILE attrs | driver/file | file_bad.pli | Partial |
| (42) | scope EXTERNAL | driver/multimod | — | Yes |
| (43) | LIKE attribute | like.pli, like_qualified.pli | bad_like.pli, bad_like_qualified.pli | Yes |
| §2.3.2.1 | keyword abbreviations | abbrev.pli | — | Yes |

### Statements (Rules 56–86)

| Rule | Feature | Core Tests | Bad Tests | Verified? |
|------|---------|------------|-----------|-----------|
| (56) | ENTRY statement | entry.pli, entry_mixed.pli | bad_entry_ret.pli | Yes |
| (60)–(63) | condition prefixes | nosize.pli | — | Yes |
| (68) | BEGIN block | begin.pli | — | Yes |
| (69)–(73) | DO groups/UNTIL/WHILE | loops.pli, do_until.pli | bad_do_until.pli, bad_leave.pli, bad_leave_label.pli | Yes |
| (74)–(76) | IF/THEN/ELSE | ifelse.pli | — | Yes |
| (77) | GO TO | goto.pli | bad_goto.pli | Yes |
| (78)–(80) | CALL/TASK/EVENT/PRIORITY | task.pli | bad_task.pli, bad_struct_dyn.pli | Partial |
| (81) | RETURN | func.pli, based_return.pli | — | Yes |
| (82),(83) | WAIT/DELAY | task.pli, task_count.pli, delay.pli | bad_task.pli | Partial |
| (84),(85) | EXIT/STOP | (covered in multiple e2e tests) | — | Yes |
| (86) | assignment (incl BY NAME) | substr_assign.pli, struct_assign.pli, multiassign.pli, struct_by_name.pli, struct_by_name_dyn.pli, array_expr.pli | bad_multiassign.pli, bad_multiassign_type.pli, bad_struct_byname.pli, bad_by_name_dyn.pli, bad_array_expr.pli | Yes |

### Conditions (Rules 91–99)

| Rule | Feature | Core Tests | Bad Tests | Verified? |
|------|---------|------------|-----------|-----------|
| (91)–(99) | ON/SIGNAL/REVERT/CONDITION | on_error.pli, on_size.pli, on_subscriptrange.pli, on_zerodivide.pli, on_cond.pli, decl_cond.pli, package_data.pli, on_return.pli, on_nested.pli | bad_on_local.pyi, bad_on_cond.pli | Yes (partial M4) |

### I/O (Rules 100–114)

| Rule | Feature | Core Tests | Bad Tests | Verified? |
|------|---------|------------|-----------|-----------|
| (100)–(103) | OPEN/CLOSE | driver/file | bad_file.pli | Partial |
| (104),(105) | GET/PUT | get.pli, put.pli, string.pli, edit.pli, e_format.pli, edit_col.pli, edit_iter.pyi, put_data.pli, get_data.pli, complex_io.pli, display.pli | bad_get.pli, bad_string.pli, bad_edit.pli, bad_col.pli, bad_col_get.pyi, bad_put_data.pli, bad_get_data.pli, bad_display.pli | Partial |
| (106)–(111) | data spec/lists/edit formats | edit_col.pyi/golden, e_format.pli, edit_iter.pyi/golden | bad_edit_iter.pli | Partial |
| (112),(113) | record I/O | driver/record | bad_record_into.pli, bad_record.pli | Partial |
| (114) | DISPLAY | display.pli | bad_display.pli | Partial |

### Expressions & Constants (Rules 115–145)

| Rule | Feature | Core Tests | Bad Tests | Verified? |
|------|---------|------------|-----------|-----------|
| (115)–(122) | expression precedence | arith.pli, usecases/expr.pli, concat_bang.pli | bad_bang.pli | Yes |
| (118) | comparison operators | complex_arith.pli | bad_complex_cmp.pli | Partial |
| (123) | primitive expressions / built-ins | array_bounds.pli, array_axis.pli, array_reduce.pli, sum_expr.pyi, struct_dyn_len.pli, pointer.pli, math.pyi, math2.pli, math3.pli, hexx.pli, trim.pli, tally.pli, sysparm.pli, array_param.pli | bad_array_builtin.pyi, bad_array_axis.pyi, bad_array_reduce.pyi, bad_sum_err.pl, bad_math*.pyl, bad_sysparm.pyi, bad_tally.yaml | Yes |
| (124),(125) | locator qualification/qual names | struct.pli, struct_array_of.pli, based.pli | bad_struct_array_of.pli | Yes |
| (126) | subscripted references | array.pli, array2d.pyi, struct_array.pil, cross_section.pli, cross_section2.pyi | bad_arr_oob.pli, bad_arr2d_oob.pyi, bad_struct_arr_*.ply, bad_cross_section.pll | Yes |
| (127) | whole-struct value | struct_return.pli, byaddr.pli, struct_assign.pli | bad_struct_assign.pli, bad_struct_value.pyi | Yes |
| (128),(129) | constants/replicated strings | strings.pli, hexx.pli, decimal.pli, complex_io.pyi | — | Yes |
| (130)–(133) | identifiers/letters/digits | keywords.pli, abbrev.pli, strings.pli, hexx.pli | — | Yes |
| (134) | iSUB | isub_defined.pli, isub_arith.pyi | bad_isub_arith.pyi, bad_isub_defined.pli | Yes |
| (135)–(139) | numeric constants | decimal.pli, complex_arith.pli | — | Yes |
| (140)–(144) | string/bit constants | strings.pyi, hexx.pyi | bad_hexx.pyi | Yes |

### Lexical (Rules 145–151)

| Rule | Feature | Core Tests | Bad Tests | Verified? |
|------|---------|------------|-----------|-----------|
| (149)–(151) | comments/whitespace | all tests (comments present throughout) | bad_include_cycle.pyi (cycle detection not lexical but related) | Yes |

### Preprocessor (C28-6571-3 Ch.9)

| Extension | Test | Verified? |
|-----------|------|-----------|
| %INCLUDE | include.pli, replace_quoted.pli | Yes |
| %REPLACE | replace.pyi, replace_quoted.pli | Yes |
| %DECLARE | pp_decl.pli | Yes |
| %IF/%THEN/%ELSE | pp.yt, pp_yes.inc, pp_no.inc | Yes |
| %ACTIVATE/%DEACTIVATE | pp_param.pli, pp_arr.inc, pp_tag.inc | Yes |
| %INCLUDE search paths | driver/include_dirs | Yes |
| Cycle detection | bad_include_cycle.pil | Yes |

### Enterprise Extensions

| Extension | Test(s) | Verified? |
|-----------|---------|-----------|
| SELECT/WHEN/OTHERWISE (ADR-104) | select.pli, bad_select.pli | Yes |
| LEAVE/ITERATE [label] (ADR-105) | leave.pli, bad_leave.pli, bad_leave_label.pli | Yes |
| DO UNTIL (ADR-106) | do_until.pli, bad_do_until.pli | Yes |
| Condition-prefix enablement (ADR-110/112) | nosize.pli, driver/nochecks | Yes |
| TRIM/TALLY (ADR-107) | trim.pli, tally.pli, bad_trim.plh, bad_tally.yli | Yes |
| VALUE named constants (ADR-108) | value.pyi, bad_value.plh, bad_value_init.pyi | Yes |
| PACKAGE/EXPORTS (ADR-109) | package.pli, driver/package, bad_package.pli, bad_package_nest.pli, package_data.pli, cousin_call.pli | Yes |
| DEFINE ALIAS/TYPE (ADR-114) | define_alias.pli, bad_define_alias.plh, bad_define_alias_combo.pyi | Yes |
| OPTIONAL parameters (ADR-119) | optional.pli, bad_optional_arg.plh, bad_optional_decl.pli, bad_omitted.pli | Yes |

---

## Rules With No Direct Tests (Diagnosed or Not Started)

These rules have no dedicated golden or self-checking tests in the suite. They may be covered indirectly via other tests exercising the parser/parser errors for those constructs:

| Rule(s) | Feature | Why no test | Alternative verification |
|---------|---------|-------------|--------------------------|
| (19) | PICTURE attribute | D1 — feature not implemented | Parser error when encountering PIC clause |
| (20)-(22) | AREA/LABEL/OFFSET|M3/M4 — deferred | Parser diagnostics accepted by test harness |
| (33) | non-data attributes | M8 — low priority | Parser rejects unknown attr names |
| (35)-(37) | ENTRY options (USEs) | Not yet explored | Parser error on USES keyword |
| (41) | GENERIC | M8 — low priority | Parser error on GENERIC keyword |
| (44)-(55) standalone FORMAT | Stubs exist for B/C/P items | M5/D1 — deferred | Parser error on unimplemented format items |
| (56) mixed return segments | Diagnosed as unimplemented | ADR-026/075 referenced in code | Error caught by bad_entry_ret.plh |
| (65) initial-label subscripted | OCR damaged in spec | M2/M4 — unclear grammar | — |
| (77) non-local GOTO | M4 — deferred | Local GOTO tested via goto.pili | — |
| (93) SIGNAL SET ONCODE | Basic signal support tested | Non-standard SIGNAL forms | — |
| (97)-(98) Named I/O conditions | Diagnostic only | No runtime behavior to test | Parser error on known condition names |
| (112)–(113) keyed/direct R/W | M6/diag — sequential only | Sequential form (driver/record) | Diagnostics verified via BAD_RECORD |
| (114) REPLy| DIAGnoised | Standalone form not parsed | Not exercised since rejected at parse |
| §2.3.1 word disambiguation | Speculative parse approach | Covered by ambiguity.pyl | Grammar ambiguity resolved correctly |
| §2.3.3 48-char translation | Partial — lexer handles some | Operator words tested in arith.pli | NOT AND OR GT LT works in practice |

---

## Test Quality Indicators

### Strengths
1. **Golden test discipline**: All core language tests assert exact output via `expected/*.out` diffs — no false positives
2. **Negative tests**: ~95 `bad_*` files cover diagnosed features with expected error messages
3. **Self-checking uSecases**: Complex programs print PASS/FAIL for integration-level assertions
4. **IR tests**: 9 LLVM IR inspection tests ensure code generation correctness
5. **Good coverage of modern features**: Dynamic arrays, cross-sections, structures, conditions, async TASK/EVENT all tested end-to-end
6. **Driver tests**: Infrastructure testing covers build, includes, sysparm, packages

### Weaknesses
1. Some edge cases not covered (negative bounds + SUBSCRIPTRANGE interaction shown insufficiently)
2. Builtins test organization could be improved (consistent naming pattern across directories)
3. Few driver-based stress/integrattion tests beyond basic path/feature checks
4. No test coverage for D1 features (PICTURE-directed I/O, picture constants)

---

_Auto-generated from GRAMMAR-COVERAGE.md and audit plan._
