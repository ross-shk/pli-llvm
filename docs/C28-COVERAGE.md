# C28-6571-3 coverage — chapter/subsection ledger

Gate 1 ledger for `QUICK-RELEASE-IMPLEMENTATION-PLAN.md`. One row per
publication chapter/named subsection (no spec prose). `TR overlap` maps to
`GRAMMAR-COVERAGE.md` rules; `QR` is the owning quick-release slice.

| Section | TR overlap | QR | Status / test |
|---|---|---|---|
| Ch.1 Program Elements (char sets, delimiters, identifiers, keywords, structure, prefixes, groups, blocks) | (1)-(10),(60)-(68),(130)-(151), §2.3 | QR1.6/QR2.4/QR2.10 | partial: `keywords.pli`, `abbrev.pli`, `begin.pli`; condition prefixes parsed only |
| Ch.2 Data Elements (arrays, structures, naming, arithmetic/string/label/task/event/pointer/area/cell) | (11)-(25),(124),(126),(127),(134) | QR1.1/QR1.3/QR2.1/QR2.2 | partial: `dyn_*`, `struct_*`, `pointer.pli`, `based.pli`, `complex_var.pli` |
| Ch.3 Data Manipulation (expressions, ops, conversions, array/struct exprs, evaluation) | (86),(115)-(123),(128),(129),(135)-(144) | QR1.1/QR1.2/QR2.1/QR2.2 | partial: `arith.pli`, `decimal.pli`, `complex_arith.pli`, `math*.pli`, `cross_section*.pli`, `struct_return.pli` |
| Ch.4 Data Description (declarations, attributes, storage, DEFINED, INITIAL, LIKE, FILE, structures) | (9)-(43) | QR1.1/QR2.1/QR2.3/QR2.6 | partial: `init_*`, `struct_init.pli`, `struct_dyn_init.pli`, `like.pli`, `defined.pli`, `driver/file` |
| Ch.5 Procedures (params, references, ENTRY, RECURSIVE, GENERIC/BUILTIN) | (2)-(5),(34)-(38),(56),(78)-(81) | QR2.6 | served (M0/M1/M2): `procs.pli`, `func.pli`, `entry*.pli`, `staticlink.pli` |
| Ch.6 Dynamic Structure (activation, storage, tasks, interrupts, ON/REVERT, checkout) | (60)-(63),(77),(82),(83),(87)-(99) | QR1.3/QR1.4/QR2.3/QR2.4/QR2.8 | partial: `alloc.pli`, `on_error.pli`, `on_cond.pli`, `on_size.pli`; prefixes unenforced |
| Ch.7 I/O (file opening, list/data/edit stream, record, files) | (39),(40),(44)-(55),(100)-(113) | QR1.5/QR2.5 | partial: `get.pli`, `string.pli`, `edit.pli`, `e_format.pli`, `display.pli`, `put_data.pli`, `get_data.pli`, `driver/file` |
| Ch.8 Statements (alphabetic list + classification) | (56)-(114) | per-slice | partial: one test per statement family; see TR rows |
| Ch.9 Compile-time (processor, DECLARE/assign/ACTIVATE/GO TO/NULL/IF/DO/INCLUDE/procedures) | suppl. | QR1.6/QR2.9 | partial: `include.pli`, `pp_if.pli`, `replace.pli` (ADR-071/077/083) |
| Ch.10 Special Topics (args/params, allocation, abnormality, list processing, built-ins) | (34)-(38) | QR1.1/QR2.1/QR2.3/QR2.6 | partial: `dyn_param.pli`, `dyn_param_lower.pli`, `star_param.pli` |
| App.1 Built-ins | (123) | QR2.7/CM5 | partial: `math*.pli`, `complex.pli`, array inquiries/reductions |
| App.2 Picture tables | (19),(146)-(148) | QR2.2 | deferred (D1, ADR-017) |
| App.3 Conditions | (91)-(99) | QR1.4/QR2.4 | partial: ERROR + SIZE + `CONDITION(name)` (`on_error.pli`, `on_size.pli`, `on_cond.pli`) |
| App.4 Abbreviations | §2.3.2.1 | QR1.6 | partial: `abbrev.pli` subset |
| App.5 48-character set | §2.3.3 | QR1.6 | partial: operator words in `arith.pli` |
| App.6 Examples | — | — | reference only |
