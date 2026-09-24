# plic — a PL/I compiler targeting LLVM

A modern PL/I compiler built to the formal specification of the language:  
**TR 25.084, *Concrete Syntax of PL/I*** (IBM Laboratory Vienna, 28 June 1968)  
for syntax, and **Y33-6003** for semantics. The extracted, OCR-repaired grammar  
lives in [`TR25.084-concrete-syntax.md`](TR25.084-concrete-syntax.md).

The original M0–M2 milestones are complete. The compiler has since gained
additional language support; the live feature matrix is
[`docs/GRAMMAR-COVERAGE.md`](docs/GRAMMAR-COVERAGE.md), and remaining 1966
language work is scheduled in the quick-release plan. LLVM IR is generated
through the LLVM C++ API (ADR-002).

```
$ make -j8
$ ./build/plic tests/core/hello.pli -o hello 
$ ./hello
Hello, world!
```

Install with:

```
make install
```

## Documentation

| Document                                                                               | Contents                                                                                                          |
| -------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------- |
| [CONTRIBUTING.md](CONTRIBUTING.md)                                                     | how to add a feature: layer map, workflow, invariants, test conventions                                           |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)                                           | pipeline, IR levels, data representation, ABI, condition model, runtime interface                                 |
| [docs/DESIGN-DECISIONS.md](docs/DESIGN-DECISIONS.md)                                   | design history: keyword handling, numeric representation, IR choices, and other decisions                       |
| [docs/OPTIMIZATION.md](docs/OPTIMIZATION.md)                                           | optimization roadmap, proposed HIR/MIR passes, LLVM integration, and measurement plan                            |
| [docs/IMPLEMENTATION-PLAN.md](docs/IMPLEMENTATION-PLAN.md)                             | compiler engineering roadmap; historical milestones and work outside language coverage                         |
| [docs/QUICK-RELEASE-IMPLEMENTATION-PLAN.md](docs/QUICK-RELEASE-IMPLEMENTATION-PLAN.md) | remaining 1966 language work, organized into practical and conformance phases                                    |
| [docs/GRAMMAR-COVERAGE.md](docs/GRAMMAR-COVERAGE.md)                                   | rule-by-rule implementation notes, supported cases, diagnostics, and test references                            |
| [docs/BUILTINS-PLAN.md](docs/BUILTINS-PLAN.md)                                         | built-in support, tests, and remaining work                                                                       |
| [docs/MODERN-PLI-PLAN.md](docs/MODERN-PLI-PLAN.md)                                     | small set of post-1966 niceties (`SELECT`, loop exits, …) as a separate extension track                           |
| [docs/POSIX-SURFACE.md](docs/POSIX-SURFACE.md)                                         | POSIX functions the `libpli` runtime relies on                                                                    |
| [docs/CROSS-PLATFORM-PLAN.md](docs/CROSS-PLATFORM-PLAN.md)                             | plan for building on other platforms (MSVC support)                                                               |

## What the compiler handles today

```pli
 hello: procedure options(main);
    put skip list('Hello, world!');
 end hello;
```

- procedures with `OPTIONS(MAIN)`, internal procedures, `CALL`, `RETURN`, `STOP`
- **function procedures** via `RETURNS(...)` and `RETURN(value)` — scalar,  
`CHAR(n) [VARYING]`, and structure results; recursive functions run with  
`RECURSIVE`, enforced across static call cycles (rules (5),(34))
- internal procedures reach enclosing automatic storage through a **static**  
**link**, so external procedures stay **reentrant** (rule (8), ADR-027)
- **multiple entry points** via the entry-namelist `a, b: PROCEDURE`  
(`multientry.pli`); sibling external procedures call each other, and  
`PACKAGE`/`EXPORTS` shares package-level data between members
- **`ENTRY` statements** (`label: ENTRY(params) RETURNS(...)`) — alternate  
entry points with their own parameters/result type (`entry.pli`, rule (56))
- calling external C procedures via `DECLARE … ENTRY(...)` (by reference,  
ADR-021), plus `OPTIONS(BYVALUE)`/`LINKAGE(SYSTEM)` value-passing for  
`FIXED`/`FLOAT` scalars and pointers (`cbyvalue.pli`)
- parameters **by reference**, with dummy arguments when conversion is needed;  
`OPTIONAL` parameters tested with `OMITTED`/`PRESENT`; `BYADDR(s)` opts a  
single structure argument out of the by-value copy
- `DECLARE` with the attribute default rules and `INITIAL` constants —  
scalars, array itemlists (iteration factors, `*` repeat-last), structures,  
`INITIAL CALL`, and dynamic extents; implicit declarations (I–N →  
`FIXED BINARY`) with warnings
- `IF`/`THEN`/`ELSE` (nested, `DO`-group branches); `BEGIN` blocks are real  
lexical scopes; `DISPLAY(scalar)`; `STOP`/`EXIT`
- `DO;`, `DO WHILE(e);`, `DO I = a TO b BY c WHILE(d);`
- **local `GO TO**` / `GOTO` to a labelled statement in the same procedure  
(`goto.pli`); non-local `GO TO` is diagnosed (M4/QR2.4)
- **multiple closure**: one `END L;` closes every open block up to `L`
- `FIXED BINARY(p,q)`, scaled `FIXED DECIMAL(p,q)`, `FLOAT`, `COMPLEX`  
(with `COMPLEX`/`REAL`/`IMAG`/`CONJG` and complex I/O), `BIT(1)` and packed  
`BIT(n)`, `CHARACTER(n)` / `VARYING`, adjustable-length `CHAR(*)`  
parameters, concatenation, blank-padded comparison, replicated string  
constants, hex `X` literals
- fixed-size arrays (multi-axis, `lb:ub`, negative bounds, row-major) and  
single-axis dynamic AUTOMATIC arrays plus dynamic/` *` parameters with  
runtime `SUBSCRIPTRANGE`/`LBOUND`/`HBOUND`/`DIM`; cross-sections  
(`A(i, *)`, `A(*, *)`) and whole-array expressions; array reductions  
`SUM`/`PROD`/`ANY`/`ALL`
- level-numbered structures with factoring, `LIKE` (incl. qualified  
templates), arrays of structures, whole-structure and `BY NAME` assignment,  
multiple assignment `a, b, c = e`
- `DEFINED` overlays with `iSUB` (incl. affine index arithmetic),  
`POINTER`/`ADDR`/`NULL`, `BASED(P)` structures with `P -> X` locators,  
`ALLOCATE … SET(P)`/`FREE`, and `CONTROLLED` generation stacks
- conditions: `ON`/`SIGNAL`/`REVERT` for `ERROR`, `SIZE`,  
`SUBSCRIPTRANGE`, `ZERODIVIDE`, and programmer-named `CONDITION(name)`;  
`ONCODE()`; `(NOSIZE)`/`(NOSUBSCRIPTRANGE)`/`(NOZERODIVIDE)` prefixes elide checks
- `TASK`/`EVENT`/`PRIORITY` async `CALL` with `WAIT`/`DELAY` synchronization
- stream I/O: `PUT`/`GET LIST` (incl. arrays, complex, decimal),  
`EDIT` with `F`/`E`/`A`/`X`/`SKIP`/`PAGE`/`LINE`/`COL` items and `(n)(…)`  
iteration groups, `DATA`-directed transmission, `STRING` and `FILE`  
routing, `OPEN`/`CLOSE` with `TITLE`
- SEQUENTIAL RECORD files: `WRITE FILE(f) FROM(v)` / `READ FILE(f) INTO(v)`  
fixed-size binary records (`driver/record`, rules (112),(113))
- preprocessor: recursive `%INCLUDE`, `%DECLARE`, `%IF … %THEN … [%ELSE]`,  
`%ACTIVATE`/`%DEACTIVATE`-gated substitution
- full operator set at spec precedence, including `**` right-associativity,  
`¬`/`^`/`~`, `!!` for concatenation, and the 48-character-set operator  
words (`AND`, `GT`, `CAT`, …)
- built-ins per [docs/BUILTINS-PLAN.md](docs/BUILTINS-PLAN.md): string  
(`SUBSTR` incl. pseudo-variable assignment, `INDEX`, `LENGTH`, `REPEAT`,  
  `VERIFY`, `TRANSLATE`, `TRIM`, `TALLY`, case/center/search/rank/collate,
  `REVERSE`, `HIGH`, `LOW`), math (incl. degree trig,
`ASIN`/`ACOS`/`ATAN2`/`CBRT`), array/pointer/misc (`LBOUND`/`HBOUND`/  
`DIM` with axis forms, `NULL`, `ADDR`, `DATE`, `TIME`, `SYSPARM`)
- **no reserved words** — `tests/core/keywords.pli` uses `IF`, `THEN`, `ELSE`, `DO`,  
`END` and `PUT` as ordinary variables

Everything else is reported as unimplemented *with its specification rule*  
*number*, which doubles as the to-do list; the full matrix is  
`docs/GRAMMAR-COVERAGE.md`.

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

`OPTIONS(BYVALUE)`/`LINKAGE(SYSTEM)` entries pass `FIXED`/`FLOAT` scalars and  
pointers as C values instead. Full dope-vector entry descriptors, `USES`/`SETS`,  
and character-valued C results are not yet implemented (QR1/QR2).

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
structure parameters need full entry descriptors, QR1.1; shared  
`EXTERNAL` variables are a follow-up).

## Usage

```
plic [options] file.pli

  -o <file>        output file (default: a.out, or <base>.ll with -emit-llvm)
  -c               compile to a relocatable object (no linking)
  -emit-llvm       write LLVM IR and stop
  --print-hir      lower to HIR and print it, then stop
  -fsyntax-only    parse and analyse only
  -O0 … -O3, -Os   optimization level passed to the LLVM pipeline (default -O2)
  --no-size-checks elide FIXED overflow traps program-wide (cf. (NOSIZE))
  --release        -O3 plus linker dead-stripping (also strips symbols on macOS)
  --debug          no optimization + debug info (-O0 -g)
  --keep-ll        keep the intermediate .ll next to the output
  --runtime <lib>  path to libpli.a (default: baked in at build time)
  --clang <path>   clang used to assemble/link the IR (default: LLVM's clang)
  --triple <t>     target triple (default: `clang -dumpmachine`)
  --sysparm <s>    value returned by the SYSPARM builtin (rule (123))
  -L <dir>         add a library search path to the link step
  -I <dir>         add a %INCLUDE search directory (repeatable; -I<dir> too)
  -l<lib>          link a library (e.g. -lm) on the link step
  -Wl,<flag>       pass a raw flag to the linker (repeatable)
  --linker <ld>    select the linker via -fuse-ld=<ld>
  -shared -static  produce a shared / static binary
  --extra <a,b,c>  comma-separated extra backend args appended to the link
  --explain <n>    print TR 25.084 rule (n)'s production and exit
  -v               show the sub-commands being run
  -h, --help       this message
```

## Layout

```
src/         compiler: diag, lexer, parser, sema, hir, irgen, preprocessor,
             explain, driver
runtime/     libpli: core, stream, string, math, conditions, storage,
             get, file, edit, task
tests/       groups (core, builtins, usecases, driver, ir): golden
             (expected/*.out) or self-checking (prints PASS) + out/
docs/        architecture, decisions, optimization, plans, coverage
```

## Building and testing

Requires a C++20 compiler and LLVM ≥ 18 with `clang` (used to  
assemble/optimize/link the generated LLVM IR — see ADR-002).

```
make -j8        # build build/plic and build/libpli.a (parallel)
make test       # compile, run and check every test program (diff or PASS-grep)
make check      # analysis gate: -Werror build + fmt-check + clang-tidy + scan-build
make clean
```

Run tests in specific groups with:

```
./tests/run_tests.py usecases
```

or a specific test with:

```
./tests/run_tests.py usecases/control.pli
```

Run `make test` for the current suite; test counts change as coverage grows.

## Known deviations

Documented in full in the ADRs; the load-bearing ones:

1. `/` and `**` are evaluated in floating point (ADR-014); exact `FIXED`
  division/scale semantics are a D1/QR2 item.
2. Unimplemented sub-cases are diagnosed with their rule number rather than
  silently accepted — the diagnostic is the to-do list (see  
   `docs/GRAMMAR-COVERAGE.md` for what is left).
