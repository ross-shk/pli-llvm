# Gap Analysis Matrix — TR 25.084 & IBM Enterprise PL/I

Generated: 2026-09-24
Compiler: plic (PL/I → LLVM)
Spec: TR 25.084 Concrete Syntax, rules (1)–(151), C28-6571-3 Ch.9
Extensions: IBM Enterprise PL/I (MX1–MX9)

## Status Legend

| Status | Meaning |
|--------|---------|
| Implemented | Fully supported with tests |
| Partial | Cases supported; unsupported cases diagnosed with rule citation |
| Diagnosed | Recognized and rejected with specification-rule citation |
| Not Started | No implementation |

---

## TR 25.084 Rules (1)–(151)

### Rules (1)–(20): Program Structure & Declarations

| # | Feature | Status | Component | Source | Tests | Deviations | Priority |
|---|---------|--------|-----------|--------|-------|------------|----------|
| (1) | `program ::= procedure•••` | Implemented | Parser | parser.cpp:parseExternalProcedure | hello.pli + all tests | — | M0 |
| (2) | procedure, entry-namelist, options | Implemented | Parser | parser.cpp:parseExternalProcedure | hello.pli | — | M0 |
| (3) | entry-namelist multi-name | Implemented | Parser/IRGen | parser.cpp; emitMultiEntryProc | multientry.pli | Extra names are internal aliases | M0 |
| (4) | parameterlist | Implemented | Parser | parser.cpp:parseDeclare | procs.pli | — | M0 |
| (5) | procedure OPTIONS, RECURSIVE, RETURNS | Implemented | Parser/Sema | parser.cpp; sema | func.pli, recursive.pli, bad_recursive*.pli, options_ignore.pli | MAIN honoured; OPTIONS(REORDER/REENTRANT/NOEXECOPS) accepted+ignored; RECURSIVE enforced via cycle analysis | M0/M1 |
| (6),(7) | sentencelist, end-clause, multiple closure | Implemented | Parser | parser.cpp:parseBody | loops.pli (`END OUTER;`) | — | M0 |
| (8) | statement kinds | Implemented | Parser | parser.cpp:parseStatement | — | Internal procs reach enclosing AUTO via static link | M0 |
| (9),(10) | DECLARE, declarationlist | Implemented | Parser | parser.cpp:parseDeclare | ifelse.pli | — | M0 |
| (11) | declarations, level numbers, factoring | Partial M2 | Parser/Sema | parser.cpp:parseDeclare | struct.pli, struct_array.pli, factor.pli, struct_charmem.pli, bad_attr_range.pli, struct_array_of.pli | INITIAL in factored form → diag; FACTOR(n) on INIT → diag; nested levels served (ADR-027); array-of-structures via member address; out-of-range attrs diagnosed | M2 |
| (12),(13) | dimension attribute, bound pairs | Partial M2 | Parser/IRGen | declareProc; arrayElementAddr | array.pli, array2d.pli, dynamic_array.pli, dyn_lower.pli, dyn_multi.pli, dyn_param.pli, star_param.pli, dyn_param_lower.pli | Negative bounds with runtime reads; single-axis DYNAMIC AUTOMATIC arrays; multi-axis with dynamic first axis; `*` adjustable-extent params; dynamic lower+upper bounds. Non-param `*` → diag | partial M2 |
| (14),(15) | data attributes (arithmetic, bit, pointer, complex, controlled) | Partial M0 | Parser/Sema | parseDeclare; ptrType, typeInfo | pointer.pli, complex_var.pli, strings.pli | AREA/M3, LABEL/M4, OFFSET/M3 unimplemented; CONTROLLED/BASED partial; POINTER equality/no-arithmetic/ordered-comparisons; COMPLEX pairs with builtins | partial M0 |
| (16),(17) | arithmetic attributes (FIXED BINARY/FLOAT) | Partial M0/M2 | IRGen | irgen.cpp:checkedArith, emitExpr | decimal.pli, scaled.pli, decimal_io.pli | FIXED BINARY overflow traps ERROR; FIXED DECIMAL rescales by 10^q round-half-away; I/O prints decimal point; float->fixed range trap (int_min edge passes). Full size routing → D1/QR2 | partial M0/M2 |
| (18) | string attributes (BIT/CHARACTER/VARYING) | Partial M0 | Parser/IRGen | declareProc, loadSym/storeTo | bitn.pli, starchar.pli, bad_starchar*.pli | BIT(n>1) arrays/stream items/mixed-length logic/wide reductions/function results → diag; CHAR(*) params served via hidden length arg (ADR-142) | partial M0 |
| (19) | PICTURE attribute | Not Started | — | — | — | D1 | D1 |

### Rules (20)–(40): Storage, Attributes, ENTRY

| # | Feature | Status | Component | Source | Tests | Deviations | Priority |
|---|---------|--------|-----------|--------|-------|------------|----------|
| (20) | AREA attribute | Not Started | — | — | — | M3 | M3 |
| (21) | LABEL attribute | Not Started | — | — | — | M4 | M4 |
| (22) | OFFSET attribute | Not Started | — | — | — | M3 | M3 |
| (23) | storage classes (AUTO/STATIC/CONTROLLED/BASED) | Partial (QR2.3 remainder) | Parser/IRGen | allocaLocals pass 2/3 | controlled.pli, implicit_controlled.pli, bad_controlled_alloc.pli | AUTO+STATIC complete; CONTROLLED generation stack partial (push/pop LIFO on proc entry); bare ALLOCATE warns; EMPTY-FREE at runtime diagnosed; MULTI-DIM/ARRAY-OF-SCTRL → M3; IN(AREA)/dynamic-based → QR2.3 | rem QR2.3 |
| (24) | DEFINED/POSITION | Partial M2 | Parser/IRGen | parseDeclare; sema | defined.pli, isub_defined.pli, isub_arith.pli, bad_defined.pli | Different-type overlays AND subscripted bases → M2; POSITION → diag (rule 24) | partial M2 |
| (25) | BASED | Partial M2 | Parser/IRGen | parseDeclare; memberAddr | based.pli, based_param.pli, based_return.pli, bad_based.pli | Bare BASED w/o pointer → diag; PROC PTR param served (ADR-143); CONTROLLED/AREA/OFFSET → M3 | partial M2 |
| (26)–(32) | INITIAL (scalar, array, call, structure, dynamic) | Partial M0→M2 | Parser/IRGen | parseDeclare; emitInitials | init_array.pli, init_call.pli, struct_init.pli, dyn_init.pli, struct_dyn_init.pli, bad_init_array.pli, bad_struct_init.pli | Factored-INITIAL → diag; STATIC-INITIAL → diag; non-trailing dynamic-member INIT → diag; ITEM-level INIT for structure members → diag | partial M0→M2 |
| (33) | non-data attributes | Not Started | — | — | — | M8 | M8 |
| (34) | ENTRY RETURNSES, descriptors, USES/SETS | Partial M0→M2 | Parser/IRGen | parseDeclare; parseEntryParams; calleeFn | entry_mixed.pli, cbyvalue.pli, starchar_entry.pli | RETU |
