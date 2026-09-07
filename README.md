# plic — a PL/I compiler targeting LLVM

A modern PL/I compiler built to the formal specification of the language:
**TR 25.084, *Concrete Syntax of PL/I*** (IBM Laboratory Vienna, 28 June 1968)
for syntax, and **Y33-6003** for semantics. The extracted, OCR-repaired grammar
lives in [`TR25.084-concrete-syntax.md`](TR25.084-concrete-syntax.md).

This tree contains the **M0 wireframe**: a small but genuinely end-to-end
compiler — PL/I source in, native executable out, via LLVM IR — together with
the architecture and plan for the full language.

```
$ make
$ ./build/plic tests/hello.pli -o hello && ./hello
Hello, world!
```

## Documentation

| Document | Contents |
|---|---|
| [CONTRIBUTING.md](CONTRIBUTING.md) | how to add a feature: layer map, workflow, invariants, test conventions |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | pipeline, IR levels, data representation, ABI, condition model, runtime interface |
| [docs/DESIGN-DECISIONS.md](docs/DESIGN-DECISIONS.md) | 20 ADRs: why no reserved words forces a hand-written parser, why decimal is scaled binary, why four IR levels, … |
| [docs/OPTIMIZATION.md](docs/OPTIMIZATION.md) | HIR/MIR passes, custom LLVM passes, metadata, `-O` levels, what we deliberately do not optimize |
| [docs/IMPLEMENTATION-PLAN.md](docs/IMPLEMENTATION-PLAN.md) | milestones M1–M9 with effort, exit criteria, risks |
| [docs/GRAMMAR-COVERAGE.md](docs/GRAMMAR-COVERAGE.md) | every rule (1)–(151) mapped to a component, test and milestone |

## What the wireframe compiles today

```pli
HELLO: PROCEDURE OPTIONS(MAIN);
   PUT SKIP LIST('Hello, world!');
END HELLO;
```

- procedures with `OPTIONS(MAIN)`, internal procedures, `CALL`, `RETURN`, `STOP`
- parameters **by reference**, with dummy arguments when conversion is needed
- `DECLARE` with the attribute default rules and `INITIAL` constants; implicit
  declarations (I–N → `FIXED BINARY`) with warnings
- `IF`/`THEN`/`ELSE` (nested, `DO`-group branches), `BEGIN` blocks
- `DO;`, `DO WHILE(e);`, `DO I = a TO b BY c WHILE(d);`
- **multiple closure**: one `END L;` closes every open block up to `L`
- `PUT [PAGE] [SKIP(n)] LIST(...)` to SYSPRINT
- full operator set at spec precedence, including `**` right-associativity,
  `¬`/`^`/`~`, and the 48-character-set operator words (`AND`, `GT`, `CAT`, …)
- `CHARACTER(n)`, `CHARACTER(n) VARYING`, concatenation, blank-padded
  comparison; `BIT(1)`; `FLOAT`; `FIXED BINARY/DECIMAL` (scale 0)
- **no reserved words** — `tests/keywords.pli` uses `IF`, `THEN`, `ELSE`, `DO`,
  `END` and `PUT` as ordinary variables

Everything else is reported as unimplemented *with its specification rule
number*, which doubles as the to-do list.

## Usage

```
plic [options] file.pli

  -o <file>        output file (default a.out)
  -emit-llvm       write LLVM IR and stop
  -fsyntax-only    parse and analyse only
  -O0 … -O3        optimization level (default -O2)
  --keep-ll        keep the intermediate .ll
  --runtime <lib>  path to libpli.a
  --triple <t>     target triple
  -v               show sub-commands
```

## Layout

```
src/         compiler: diag, lexer, parser, sema, irgen, driver
runtime/     libpli: list-directed output, string semantics, conditions
tests/       .pli programs + expected output + run_tests.sh
docs/        architecture, decisions, optimization, plan, coverage
```

## Building and testing

Requires a C++20 compiler and `clang` (used to assemble/optimize/link the
generated LLVM IR — see ADR-002; the LLVM C++ API arrives in M1).

```
make          # build build/plic and build/libpli.a
make test     # compile, run and diff every test program
make clean
```

Current suite: 9 tests (8 execution + 1 diagnostic), all passing.

## Example: generated IR

```
$ ./build/plic tests/hello.pli -emit-llvm -o hello.ll && cat hello.ll
define internal void @PLI_HELLO() {
entry:
  call void @pli_put_skip(i64 1)
  call void @pli_put_list_char(ptr @.str.0, i64 13)
  ret void
}
define i32 @main() {
entry:
  call void @pli_rt_init()
  call void @PLI_HELLO()
  call void @pli_rt_fini()
  ret i32 0
}
```

## Known deviations in M0

Documented in full in the ADRs; the load-bearing ones:

1. `/` and `**` are evaluated in floating point (ADR-014); exact `FIXED`
   precision/scale arrives in M2.
2. Variables of the external procedure get static storage so internal
   procedures can see them without a static link (ADR-010); M1 fixes this.
3. `PUT SKIP` on a fresh line does not emit a blank line, unlike a real
   SYSPRINT whose page/line position is tracked (M6).
