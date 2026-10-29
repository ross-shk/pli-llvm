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

The release publishes both `pli-windows-x86_64.zip` and `pli-windows-x86_64.tar.gz`. The archive contains a top-level `windows-x86_64/` dir holding `pli.exe`.

cmd.exe:

```bat
curl -L https://github.com/ross-shk/pli-llvm/releases/latest/download/pli-windows-x86_64.zip -o pli.zip
if not exist "%USERPROFILE%\pli\windows-x86_64" mkdir "%USERPROFILE%\pli\windows-x86_64"
tar -xf pli.zip -C "%USERPROFILE%\pli"
setx PATH "%PATH%;%USERPROFILE%\pli\windows-x86_64"
```

PowerShell:

```pwsh
Invoke-WebRequest -Uri https://github.com/ross-shk/pli-llvm/releases/latest/download/pli-windows-x86_64.zip -OutFile pli.zip
Expand-Archive -Force pli.zip -DestinationPath $env:USERPROFILE\pli
[Environment]::SetEnvironmentVariable("PATH", $env:PATH + ";$env:USERPROFILE\pli\windows-x86_64", [EnvironmentVariableTarget]::User)
```

Then open a new shell so `%USERPROFILE%\pli\windows-x86_64` is on `PATH` and `pli.exe` resolves.

Each archive contains just the compiler binary (`pli` or `pli.exe`); the runtime and platform link sysroot are embedded.

## Hello, world

```pli
hello: procedure options(main);
  declare name char(40);
  
  put skip list('Enter your name:');
  get list(name);
  put skip list('Hello, ' || name '!');
end;
```

```sh
pli hello.pli -o hello
./hello
# When running, enter your name at the prompt, e.g. "Alice"
```

For a fuller tour — arrays, structures, storage, conditions, concurrency — see the [Programming Guide](guides/programming.md). Supported built-ins are in the [Built-ins reference](guides/builtins.md).

## Benchmarks

`benchmarks/run_benchmarks.py` compares PL/I against C across 33 micro-benchmarks (scalar, string, decimal, array, I/O); `benchmarks/run_tasking_bench.py` compares the tasking models against Go and C. Methodology and the validity rules are in [`benchmarks/README.md`](benchmarks/README.md).

```sh
cmake --build build/cmake -j                   # rebuild before timing
python3 benchmarks/run_benchmarks.py
python3 benchmarks/run_benchmarks.py --c-musl  # Linux: C twin on the same static musl
python3 benchmarks/run_tasking_bench.py --models fiber,os
```

### PL/I vs C (micro-benchmarks, -O2, median)

22 of 32 comparable benchmarks are at parity with or faster than the matched C baseline, and every row is within 1.06×. The IR corpus geomean is 0.75× (acceptance gate < 0.90×); the language corpus geomean is 0.57×.

Measured on macOS 26 arm64, five samples per side, unpinned. The C twin uses the same target policy as the PL/I output (generic CPU, no FP contraction); `--c-native` builds it for the host CPU instead. Rows whose PL/I median is under 50 ms are indicative.

On x86-64, with alternating PL/I/C samples, the IR geomean is **0.84× on Windows** (gate passes) and **0.96× on Linux** against the host-glibc C twin. `--c-musl` builds the C twin with the same static musl libc as the Linux PL/I output; that fair baseline is **0.89×** (gate passes). Per-row numbers and run IDs are in [`docs/OPTIMIZATION-BASELINE.md`](docs/OPTIMIZATION-BASELINE.md) §15.

**IR corpus** (C parity — the acceptance gate):

| Benchmark | Ratio | What it measures |
|---|---:|---|
| `string_search` | 0.07× | Searches for matches in a 76 KB text buffer |
| `string_ops` | 0.22× | Joins text and extracts substrings |
| `decimal_ops` † | 0.59× | Decimal-number arithmetic (fixed-point style) |
| `controlled_stack` | 0.73× | Repeated push/fill/sum/pop on a stack-like array |
| `bcd_decimal` | 0.78× | Decimal arithmetic using a BCD-style representation |
| `array_sort` | 0.80× | Bubble-sorts 18,000 integers |
| `condition_ops` | 0.81× | Array access with built-in error handling for bad indexes |
| `fixed_bin63` | 0.82× | 63-bit integer math |
| `io_throughput` | 0.84× | Prints 200,000 output lines |
| `scalar_arith` | 0.85× | Integer and floating-point math on single values |
| `file_io` | 0.95× | Writes then reads 500,000 records in order |
| `on_conditions` | 0.96× | Repeatedly handles divide-by-zero during division |
| `collatz` | 0.98× | Runs the Collatz sequence for 2 to 400,000 |
| `mandelbrot` | 1.00× | Computes Mandelbrot iterations for a 1200×800 image |
| `bit_ops` | 1.01× | AND/OR/NOT on 32-bit values |
| `division` | 1.01× | Fixed-point division using a divisor derived from modulo |
| `matrix_float` | 1.02× | Multiplies a 400×400 matrix (with trig/math seeding) |
| `math_functions` | 1.02× | Runs math library functions inside nested loops |
| `proc_calls` | 1.03× | Repeated function calls (including recursive and iterative) |
| `linked_list` | 1.04× | Builds, walks, and frees a list with 3 million nodes |
| `array_matmul` | 1.05× | Multiplies two 512×512 integer matrices |

**Language corpus** (PL/I semantics a C program cannot express; the C twin is a reference):

| Benchmark | Ratio | What it measures |
|---|---:|---|
| `select_chain` | 0.09× | Picks from many constant choices |
| `picture_fold` | 0.19× | Converts a formatted constant value |
| `decimal_scale` | 0.44× | Adjusts decimal scale and rounds correctly |
| `by_name` | 0.47× | Copies a group of fields using name-based mapping |
| `pure_call` | 0.65× | Lets the compiler reuse a helper call safely inside a loop |
| `io_list` | 0.69× | Writes a list of mixed value types |
| `concat_const` | 0.85× | Builds a final string using fixed offsets |
| `checks_on` | 0.85× | Does array and overflow checks that can’t be skipped |
| `bit_merge` | 0.98× | Combines two masked bit tests into one |
| `alias_scope` | 0.98× | Copies arrays efficiently when it’s safe they don’t overlap |
| `on_dispatch` | 1.02× | Handles divide-by-zero with a multi-step handler |
| `bit_fields` | 1.05× | Tests multiple masked 32-bit fields |

† `decimal_ops` is descriptive: its PL/I side is fixed decimal and its C side is `double`, so the row informs but does not gate. Every row matches its C twin exactly, except the float-sensitive rows (`scalar_arith`, `math_functions`, `matrix_float`), which are compared with a tolerance and also match.

Full results and the methodology live in `benchmarks/`.

### PL/I vs Go (tasking, fiber model)

<table class="markdown-table">
  <tr><th>Benchmark</th><th>PL/I / Go ratio</th><th>Result</th></tr>
  <tr><td>task_loop (spawn–join lifecycle)</td><td>0.69×</td><td>PL/I beats Go</td></tr>
  <tr><td>task_spawn (20 000 live tasks)</td><td>1.1×–1.5×</td><td>Near parity: peers wake during the spawn loop and recycle stacks (ADR-324/325)</td></tr>
</table>

The fiber model recycles a stack per task, making it faster than Go on the spawn–join lifecycle. A fan-out now wakes idle peers while it is still spawning, so finished tasks hand their stacks back instead of every task keeping one: 20 000 tasks peak at ~10 MiB, and the remaining gap to Go is its smaller copied stack.

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

Build with `clang-cl` + `lld-link` (not `cl.exe`, MinGW, MSYS2, Cygwin, or WSL). First set up a Visual Studio developer environment (the Windows SDK is required). `vcvarsall.bat` location varies by VS edition/year — locate it with `vswhere` or use the common fallback paths:

cmd.exe:

```bat
REM Locate vcvarsall.bat (examples; adjust to your install)
for %i in (
  "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
  "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
  "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
) do if exist "%~fi" call "%~fi" x64

cmake -G Ninja -S . -B build/cmake ^
  -DCMAKE_PREFIX_PATH=C:/path/to/llvm-23 ^
  -DCMAKE_C_COMPILER=clang-cl ^
  -DCMAKE_CXX_COMPILER=clang-cl ^
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake -j
```

PowerShell:

```pwsh
# Locate vcvarsall.bat (examples; adjust to your install)
$vcvars = @(
  "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat",
  "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat",
  "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vcvars) { throw "vcvarsall.bat not found; install the Visual Studio C++ Build Tools" }
cmd /c """$vcvars"" x64 & set" | ForEach-Object {
  if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], [EnvironmentVariableTarget]::Process) }
}

cmake -G Ninja -S . -B build/cmake `
  -DCMAKE_PREFIX_PATH=C:/path/to/llvm-23 `
  -DCMAKE_C_COMPILER=clang-cl `
  -DCMAKE_CXX_COMPILER=clang-cl `
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake -j
```

Use clang-cl + lld-link, forward slashes in CMake paths, and make sure Ninja is on `PATH`.

### Hermetic static (release builds)

Use the pinned static LLVM instead of a distro install:

cmd.exe (Git Bash/MSYS):

```sh
sh scripts/fetch_llvm_static.sh --prefix ~/pli-deps
cmake -G Ninja -S . -B build/cmake -DPLI_LLVM_STATIC_PREFIX=~/pli-deps
cmake --build build/cmake -j
```

PowerShell (native, no Git Bash required):

```pwsh
.\scripts\fetch_llvm_static.ps1 --prefix $env:USERPROFILE\pli-deps
cmake -G Ninja -S . -B build/cmake -DPLI_LLVM_STATIC_PREFIX=$env:USERPROFILE\pli-deps
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
