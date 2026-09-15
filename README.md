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
$ ./build/plic tests/core/hello.pli -o hello && ./hello
Hello, world!
```

Install with:

```
make install               
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
 hello: procedure options(main);
    put skip list('Hello, world!');
 end hello;
```

- procedures with `OPTIONS(MAIN)`, internal procedures, `CALL`, `RETURN`, `STOP`
- **function procedures** via `RETURNS(...)` and `RETURN(value)`, used as
  value-producing expressions; recursive functions run with `RECURSIVE`
  (`func.pli`, `recursive.pli`, rule (5))
- internal procedures reach a variable of an enclosing procedure through a
  **static link** (ADR-027); variables are `AUTOMATIC`, so external procedures
  are **reentrant** (`staticlink.pli`, rule (8))
- **multiple entry points** via the entry-namelist `a, b: PROCEDURE` — the body
  is reachable through any name (`multientry.pli`); sibling external
  procedures can call each other (program scope)
- **`ENTRY` statements** (`label: ENTRY(params) RETURNS(...)`) — an alternate
  entry point with its own parameters/result type; execution starts at that
  `ENTRY` (`entry.pli`, rule (56))
- calling external C procedures via `DECLARE … ENTRY(...)` (by reference, ADR-021)
- parameters **by reference**, with dummy arguments when conversion is needed
- `DECLARE` with the attribute default rules and `INITIAL` constants; implicit
  declarations (I–N → `FIXED BINARY`) with warnings
- `IF`/`THEN`/`ELSE` (nested, `DO`-group branches); `BEGIN` blocks are real
  lexical scopes (inner declarations shadow outer ones; ADR-023)
- `DO;`, `DO WHILE(e);`, `DO I = a TO b BY c WHILE(d);`
- **local `GO TO`** / `GOTO` to a labelled statement in the same procedure
  (`goto.pli`); non-local `GO TO` is M5
- **multiple closure**: one `END L;` closes every open block up to `L`
- `PUT [PAGE] [SKIP(n)] LIST(...)` to SYSPRINT; `PUT DATA(a, ...)`
  writes each variable as `NAME=value`, `", "`-separated and `;`-terminated,
  and `GET DATA(a, ...)` reads such pairs back in any order
- SEQUENTIAL RECORD files: `OPEN FILE(f) RECORD SEQUENTIAL ...` plus
  `WRITE FILE(f) FROM(v)` / `READ FILE(f) INTO(v)` fixed-size binary
  records (`driver/record`, rules (112),(113))
- full operator set at spec precedence, including `**` right-associativity,
  `¬`/`^`/`~`, and the 48-character-set operator words (`AND`, `GT`, `CAT`, …)
- `CHARACTER(n)`, `CHARACTER(n) VARYING`, concatenation, blank-padded
  comparison; replicated string constants `(n)'str'` (rule 129); the `SUBSTR`,
  `INDEX`, `LENGTH`, `REPEAT`, `VERIFY`, `TRANSLATE`, `HIGH`, `LOW`, `DATE`,
  `TIME`, `ABS`, `TRUNC`, `MIN`, `MAX`, `MOD`, `ROUND`, `MULTIPLY`, `DIVIDE`,
  `PRECISION`, `FLOOR`, `CEIL`, `SQRT`, `EXP`, `LOG`, `LOG2`, `LOG10`, `ATAN`,
  `SINH`, `COSH`, `TANH`, `ATANH`, `ERF`, `ERFC`, `SIN`, `COS`, `TAN`, and the
  degree trig `SIND`, `COSD`, `TAND`, `ATAND`, and the complex component/
  conjugate `COMPLEX`, `REAL`, `IMAG`, `CONJG` built-in functions;
  `SUBSTR(v, i, n) = x` pseudo-variable
  (ADR-024); `BIT(1)`; `FLOAT`; `FIXED BINARY/DECIMAL` (scale 0);
  `COMPLEX` variables (`DECLARE z COMPLEX;`) holding a real+imaginary pair
  (ADR-074)
- **no reserved words** — `tests/core/keywords.pli` uses `IF`, `THEN`, `ELSE`, `DO`,
  `END` and `PUT` as ordinary variables

Everything else is reported as unimplemented *with its specification rule
number*, which doubles as the to-do list.

## Calling external C procedures

PL/I can call procedures written in C (architecture goal 4; ADR-021). Declare
an external entry with the `ENTRY` attribute (rule 38) and call it like any
procedure; arguments are passed **by reference** — the PL/I default — so the
C callee receives pointers.

`caller.pli`:

```pli
 caller: procedure options(main);
    declare x fixed bin(31);
    declare c_set entry (fixed bin(31))
       external('c_set');
    x = 0;
    call c_set(x);            /* C function receives &x */
    if x = 42 then put skip list('PASS');
 end caller;
```

`c_set.c`:

```c
void c_set(int *x) { *x = 42; }
```

Compile each unit to an object and link them together with the runtime:

```
./build/plic caller.pli -c -o caller.o
cc -c c_set.c -o c_set.o
cc caller.o c_set.o build/libpli.a -o caller
```

Full entry descriptors, `USES`/`SETS`, character-valued results, and
`OPTIONS(BYVALUE)` value-passing are not yet implemented (M2 / M9).

## Multi-module PL/I programs

A top-level (non-`MAIN`, non-nested) procedure is externally linked
under its upper-cased name (rule 42; ADR-103), so another unit's
`ENTRY...EXTERNAL` declaration resolves at link time. Compile each
unit with `-c` and link the objects with the runtime:

```
./build/plic main.pli -c -o main.o
./build/plic lib.pli -c -o lib.o
cc main.o lib.o build/libpli.a -o prog
```

Scalar parameters are passed by reference and function returns work
across the link, exactly as in the C-interop form above (array and
structure parameters need full entry descriptors, M2; shared
`EXTERNAL` variables are a follow-up).

## Usage

```
plic [options] file.pli

  -o <file>        output file (default a.out)
  -c               compile to a relocatable object (no linking)
  -emit-llvm       write LLVM IR and stop
  -fsyntax-only    parse and analyse only
  -O0 … -O3        optimization level (default -O2)
  --keep-ll        keep the intermediate .ll
  --runtime <lib>  path to libpli.a
  --triple <t>     target triple
  -L <dir>         add a library search path to the link step
  -l<lib>          link a library on the link step
  -Wl,<flag>       pass a raw flag to the linker
  --linker <ld>    select the linker via -fuse-ld=<ld>
  -shared -static  produce a shared / static binary
  --extra <a,b,c>  comma-separated extra backend args
  --explain <n>    print TR 25.084 rule (n)'s production and exit
  -v               show sub-commands
```

## Layout

```
src/         compiler: diag, lexer, parser, sema, irgen, driver
runtime/     libpli: list-directed output, string semantics, conditions
tests/       groups: golden (expected/*.out) or self-checking (prints PASS) + out/
docs/        architecture, decisions, optimization, plan, coverage
```

## Building and testing

Requires a C++20 compiler and `clang` (used to assemble/optimize/link the
generated LLVM IR — see ADR-002; the LLVM C++ API arrives in M1).

```
make          # build build/plic and build/libpli.a
make test     # compile, run and check every test program (diff or PASS-grep)
make check    # analysis gate: -Werror build + fmt-check + clang-tidy + scan-build
make clean
```

Run tests in specific groups with:

```
`./tests/run_tests.py usecases`
```

or a specific test with:

```
`./tests/run_tests.py usecases/control.pli`
```

Current suite: 50 tests (9 golden + 24 self-contained + 17 diagnostic), all passing.

## Example: generated IR

```
$ ./build/plic tests/core/hello.pli -emit-llvm -o hello.ll && cat hello.ll
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
4. Function procedures return scalar (numeric/`BIT`) results by value
   (ADR-022); character-valued results are diagnosed as unimplemented (M2).
