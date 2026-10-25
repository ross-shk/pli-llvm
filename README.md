# pli-llvm — a PL/I compiler targeting LLVM

`pli` is a modern PL/I compiler built to **TR 25.084** (Concrete Syntax) and **Y33-6003** (Semantics). It implements roughly 80% of TR 25.084, plus selected IBM Enterprise PL/I extensions. It ships as a single self-contained binary, so compiling and linking user programs need no LLVM install, Xcode, build-essential, or MSVC.

## Install

Download the [latest release](https://github.com/ross-shk/pli-llvm/releases/latest) and put `pli` on your `PATH`.

Linux (x86_64 or aarch64) and macOS (Apple Silicon):

```sh
os=$(uname -s | tr '[:upper:]' '[:lower:]')
arch=$(uname -m)          
curl -L "https://github.com/ross-shk/pli-llvm/releases/latest/download/pli-$os-$arch.tar.gz" \
  | tar xzf - && sudo install -m 755 "$os-$arch/pli" /usr/local/bin/pli
```

Windows (x86_64):

```bat
curl -L https://github.com/ross-shk/pli-llvm/releases/latest/download/pli-windows-x86_64.zip -o pli.zip
if not exist "%USERPROFILE%\pli" mkdir "%USERPROFILE%\pli"
tar -xf pli.zip -C "%USERPROFILE%\pli"
```

Then add `%USERPROFILE%\pli` to `PATH`.

Each archive contains just the compiler binary (`pli` or `pli.exe`); the runtime and platform link sysroot are embedded.

## Hello, world

```pli
hello: procedure options(main);
  put skip list('Hello, world!');
end hello;
```

```sh
pli hello.pli -o hello
./hello
```

For a fuller tour — arrays, structures, storage, conditions, concurrency — see the [Programming Guide](guides/programming.md). Supported built-ins are in the [Built-ins reference](guides/builtins.md).

## Benchmarks

`benchmarks/run_benchmarks.py` compares PL/I against C across 33 micro-benchmarks (scalar, string, decimal, array, I/O); `benchmarks/run_tasking_bench.py` compares the tasking models against Go and C. Methodology and the validity rules are in [`benchmarks/README.md`](benchmarks/README.md).

```sh
cmake --build build/cmake -j                   # rebuild before timing
python3 benchmarks/run_benchmarks.py
python3 benchmarks/run_tasking_bench.py --models fiber,os
```

### PL/I vs C (micro-benchmarks, -O2, median)

22 of 32 comparable benchmarks are at parity with or faster than the matched C baseline, and every row is within 1.06×. The IR corpus geomean is 0.75× (acceptance gate < 0.90×); the language corpus geomean is 0.57×. The wins concentrate where PL/I's native semantics avoid per-element C overhead.

Measured on macOS 26 arm64, five samples per side, unpinned. The C twin uses the same target policy as the PL/I output (generic CPU, no FP contraction); `--c-native` builds it for the host CPU instead.

**IR corpus** (C parity — the acceptance gate):

| Benchmark | PL/I (ms) | C (ms) | Ratio |
|---|---:|---:|---:|
| `string_search` | 84.56 | 1272.62 | 0.07× |
| `string_ops` | 4.10 | 18.90 | 0.22× |
| `decimal_ops` † | 37.88 | 64.43 | 0.59× |
| `controlled_stack` | 26.36 | 36.12 | 0.73× |
| `bcd_decimal` | 103.12 | 131.70 | 0.78× |
| `array_sort` | 59.94 | 75.15 | 0.80× |
| `condition_ops` | 72.49 | 89.11 | 0.81× |
| `fixed_bin63` | 67.48 | 82.29 | 0.82× |
| `io_throughput` | 30.66 | 36.59 | 0.84× |
| `scalar_arith` | 53.55 | 62.71 | 0.85× |
| `file_io` | 58.76 | 61.54 | 0.95× |
| `on_conditions` | 86.12 | 89.77 | 0.96× |
| `collatz` | 57.23 | 58.16 | 0.98× |
| `mandelbrot` | 59.18 | 59.01 | 1.00× |
| `bit_ops` | 83.70 | 83.25 | 1.01× |
| `division` | 55.40 | 54.79 | 1.01× |
| `matrix_float` | 61.83 | 60.61 | 1.02× |
| `math_functions` | 57.57 | 56.33 | 1.02× |
| `proc_calls` | 29.28 | 28.34 | 1.03× |
| `linked_list` | 80.31 | 77.07 | 1.04× |
| `array_matmul` | 109.62 | 104.03 | 1.05× |

**Language corpus** (PL/I semantics a C program cannot express; the C twin is a reference):

| Benchmark | PL/I (ms) | C (ms) | Ratio |
|---|---:|---:|---:|
| `select_chain` | 31.74 | 352.37 | 0.09× |
| `picture_fold` | 18.89 | 99.52 | 0.19× |
| `decimal_scale` | 59.89 | 136.07 | 0.44× |
| `by_name` | 3.73 | 7.88 | 0.47× |
| `pure_call` | 63.97 | 99.06 | 0.65× |
| `io_list` | 95.46 | 138.12 | 0.69× |
| `concat_const` | 5.51 | 6.52 | 0.85× |
| `checks_on` | 71.19 | 83.36 | 0.85× |
| `bit_merge` | 74.26 | 75.58 | 0.98× |
| `alias_scope` | 54.07 | 54.98 | 0.98× |
| `on_dispatch` | 47.11 | 45.97 | 1.02× |
| `bit_fields` | 69.91 | 66.47 | 1.05× |

† `decimal_ops` is descriptive: its PL/I side is fixed decimal and its C side is `double`, so the row informs but does not gate. Every row matches its C twin exactly, except the float-sensitive rows (`scalar_arith`, `math_functions`, `matrix_float`), which are compared with a tolerance and also match.

Full results and the methodology live in `benchmarks/`.

### PL/I vs Go (tasking, fiber model)

<table class="markdown-table">
  <tr><th>Benchmark</th><th>PL/I / Go ratio</th><th>Result</th></tr>
  <tr><td>task_loop (spawn–join lifecycle)</td><td>0.69×</td><td>PL/I beats Go</td></tr>
  <tr><td>task_spawn (20 000 live tasks)</td><td>10.8× (was 24.8×)</td><td>Go's 2 KiB growable stack still wins for fan-out</td></tr>
</table>

The fiber model recycles a stack per task, making it faster than Go on the spawn–join lifecycle. Fan-out of many simultaneously-live tasks is PL/I's known cost: a fiber stack comes from an arena and grows in chunks, so its 8 MiB ceiling costs address space only, but every live task still touches one 16 KiB stack page.

## Build from source

Builds on Linux (x86_64, ARM64), macOS (Intel, Apple Silicon), and Windows (x86_64).

| Requirement | Minimum | Notes |
| --- | --- | --- |
| C++20 compiler | gcc 12 / clang 15 / clang-cl 18+ | Windows also needs the Windows 10/11 SDK |
| CMake | 3.20 | All platforms |
| Ninja | recent | Canonical generator on all platforms |
| LLVM + clang + lld | 23 | Development libraries and lld headers/libs |
| Python | 3 | Build scripts and test runner |
| Git | recent | Optional; without it `pli version` reports `unknown` |

Use distro LLVM for development, or the pinned static toolchain for release builds.

### Linux / macOS

```sh
cmake -G Ninja -S . -B build/cmake -DCMAKE_PREFIX_PATH=/path/to/llvm-23
cmake --build build/cmake -j
```

On macOS, use `-DCMAKE_PREFIX_PATH="$(brew --prefix llvm)"`. On Linux, the distro package is usually found automatically; otherwise point at `/usr/lib/llvm-23`.

### Windows

Run from `cmd.exe`, not PowerShell or a MinGW/MSYS shell:

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64

cmake -G Ninja -S C:/path/to/pli-llvm -B C:/path/to/pli-llvm/build/cmake ^
  -DCMAKE_PREFIX_PATH=C:/path/to/llvm-23 ^
  -DCMAKE_C_COMPILER=clang-cl ^
  -DCMAKE_CXX_COMPILER=clang-cl ^
  -DCMAKE_BUILD_TYPE=Release
cmake --build C:/path/to/pli-llvm/build/cmake -j
```

Use clang-cl + lld-link (not `cl.exe`, MinGW, MSYS2, Cygwin, or WSL), forward slashes in CMake paths, and make sure Ninja is on `PATH`.

### Hermetic static (release builds)

Use the pinned static LLVM instead of a distro install:

```sh
sh scripts/fetch_llvm_static.sh --prefix ~/pli-deps
cmake -G Ninja -S . -B build/cmake -DPLI_LLVM_STATIC_PREFIX=~/pli-deps
cmake --build build/cmake -j
```

Rebuild the toolchain only when bumping `scripts/llvm_static.pins`:

```sh
sh scripts/build_llvm_static.sh --prefix ~/pli-deps --pack out.tar.zst
```

### Install the build

```sh
cmake --install build/cmake
```

Add `--prefix /opt/pli` to install elsewhere. The default prefix may need `sudo` or an elevated shell.

## Test

```sh
ctest --test-dir build/cmake -j              # full suite
python3 tests/run_tests.py usecases          # one group
cmake --build build/cmake --target check     # formatting + static analysis + -Werror
```

## Cross-compilation

Configure with `PLI_CROSS_BITCODE=ON` to embed all four runtime bitcodes:

```sh
cmake -G Ninja -S . -B build/cmake -DPLI_CROSS_BITCODE=ON   
cmake --build build/cmake -j

./build/cmake/pli --triple x86_64-unknown-linux-gnu program.pli -o program
./build/cmake/pli --triple x86_64-pc-windows-gnu program.pli -o program.exe
```

Supported triples: `x86_64-unknown-linux-gnu`, `aarch64-unknown-linux-gnu`, `x86_64-pc-windows-gnu`, `aarch64-pc-windows-gnu`. Without `PLI_CROSS_BITCODE=ON`, `--triple` uses the host runtime and only works when the target ABI matches the host.

## What works today

- **Procedures & functions** — `OPTIONS(MAIN)`, internal/external procedures, `RETURNS`, `RECURSIVE`, `ENTRY`, `PACKAGE`/`EXPORTS`.
- **Data types** — `FIXED BIN/DEC`, `FLOAT`, `COMPLEX`, `BIT`, `CHAR`/`VARYING`, `POINTER`, `AREA`/`OFFSET`, `VALUE`, `DEFINE ALIAS/TYPE`.
- **Arrays & structures** — dynamic/adjustable bounds, negative bounds, cross-sections, whole-array expressions, reductions, arrays of structures, `LIKE`, `BY NAME`.
- **Storage** — `BASED`/`ALLOCATE`/`FREE`, `CONTROLLED`, `DEFINED`/`iSUB`, `ADDR`/`NULL`.
- **Conditions** — `ON`/`SIGNAL`/`REVERT`, standard and named conditions, `(NO...)`, `ONCODE()`, nested `ON`.
- **Concurrency** — `TASK`/`EVENT`/`PRIORITY` async `CALL`, `WAIT`/`DELAY`, `EVENT()` polling.
- **I/O** — `PUT`/`GET LIST`/`EDIT`/`DATA`, `FILE`/`STRING`, `OPEN`/`CLOSE`, `FORMAT`, sequential record I/O.
- **Preprocessor** — `%INCLUDE`/`%XINCLUDE`, `%DECLARE`, `%IF`/`%THEN`/`%ELSE`, `%ACTIVATE`, `%REPLACE`.
- **C interop** — `ENTRY ... EXTERNAL`, `BYVALUE`/`BYADDR`, `LINKAGE(SYSTEM)`, `OPTIONAL`/`OMITTED`, `ENTRY VARIABLE`, `UNALIGNED`, `SYSTEM()`.
- **Toolchain** — separate compilation, multi-unit `pli` invocation, `-c`/`-emit-llvm`, `-I`/`-L`, `--sysparm`, `--triple`.
- **Built-ins** — string, conversion, math, complex, and array-bound built-ins; full list in the [Built-ins reference](guides/builtins.md).
- **Extensions** — `SELECT`/`WHEN`/`OTHERWISE`, `LEAVE`/`ITERATE`, `DO UNTIL`, `//` comments.
- **No reserved words** — `IF`, `THEN`, `ELSE`, `DO`, `END`, `PUT` can be ordinary variables.

Everything else is diagnosed with its TR 25.084 rule number — the diagnostic is the to-do list. Larger examples live in `tests/usecases/` and `benchmarks/`.

## Documentation

| Doc | Purpose |
| --- | --- |
| [CONTRIBUTING.md](CONTRIBUTING.md) | How to add features: layer map, workflow, invariants |
| [guides/programming.md](guides/programming.md) | Feature guide: data types, arrays, storage, I/O, conditions, concurrency |
| [guides/builtins.md](guides/builtins.md) | Built-in functions reference |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Pipeline, IR levels, data representation, ABI, runtime |
| [docs/GRAMMAR-COVERAGE.md](docs/GRAMMAR-COVERAGE.md) | Rule-by-rule implementation status |
| [docs/DESIGN-DECISIONS.md](docs/DESIGN-DECISIONS.md) | Historical design decisions (ADRs) |
| [TR25.084-concrete-syntax.md](TR25.084-concrete-syntax.md) | TR 25.084 concrete syntax, rules (1)–(151) |

## Layout

| Path | Contents |
| --- | --- |
| `src/` | Compiler pipeline: lexer, parser, sema, HIR, IRGen, preprocessor, diagnostics, target |
| `runtime/` | PL/I runtime and platform seam |
| `tests/` | Golden, self-checking, and driver tests |
| `benchmarks/` | Benchmark programs, runner, and results |
| `scripts/` | Build and code-generation helpers |
| `docs/`, `guides/` | Architecture, ADRs, grammar coverage, user guides |
| `CMakeLists.txt` | Canonical build: LLVM C++ API + lld, embedded runtime bitcode |

## License

Apache 2.0 — see `LICENSE`.
