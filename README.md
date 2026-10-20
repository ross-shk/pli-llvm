# pli-llvm — a PL/I compiler targeting LLVM

`pli` is a modern PL/I compiler built to **TR 25.084** (Concrete Syntax) and **Y33-6003** (Semantics). It implements roughly 80% of TR 25.084, plus selected IBM Enterprise PL/I extensions. It ships as a single self-contained binary, so compiling and linking user programs need no LLVM install, Xcode, build-essential, or MSVC.

## Install

Download the [latest release](https://github.com/ross-shk/pli-llvm/releases/latest) and put `pli` on your `PATH`.

Linux (x86_64 or aarch64) and macOS (Apple Silicon):

```sh
os=$(uname -s | tr '[:upper:]' '[:lower:]')
arch=$(uname -m)          # x86_64, aarch64, or arm64
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

`pli --triple <t>` compiles for another target: the PL/I runtime is compiled
for that target too, and the program is linked against that target's C
runtime.

| Target | Needs |
| --- | --- |
| `x86_64-unknown-linux-gnu` | a musl sysroot (see below) |
| `aarch64-unknown-linux-gnu` | a musl sysroot |
| `x86_64-pc-windows-gnu` | a mingw-w64 sysroot |
| `aarch64-apple-macosx15.0` | the macOS SDK of the build host |

`aarch64-pc-windows-gnu` is **not** supported: mingw-w64 has no aarch64 port,
so no sysroot exists for it. The compiler says so instead of failing later.

### Prepare a sysroot

A cross target needs the target's C library headers (to build the runtime)
and its CRT objects and libraries (to link). The script builds both in a
container and installs them in a local cache:

```sh
sh scripts/prepare_cross_sysroot.sh x86_64-unknown-linux-gnu
sh scripts/prepare_cross_sysroot.sh aarch64-unknown-linux-gnu
sh scripts/prepare_cross_sysroot.sh x86_64-pc-windows-gnu
```

It needs Docker; the sysroots land in `~/.pli/sysroots/<target>` (override
with `PLI_SYSROOT_DIR`). A macOS target needs no preparation: the SDK of the
build host is used, so only a macOS build host can target macOS.

### Build and use

```sh
cmake -G Ninja -S . -B build/cmake -DPLI_CROSS_BITCODE=ON   # plus platform flags
cmake --build build/cmake -j

./build/cmake/pli --triple x86_64-unknown-linux-gnu program.pli -o program
./build/cmake/pli --triple x86_64-pc-windows-gnu program.pli -o program.exe
./build/cmake/pli --triple aarch64-unknown-linux-gnu program.pli -c -o program.o
```

`pli version` lists the targets this build can compile for. A target without
a runtime bitcode is diagnosed, never served with the host's runtime:

```
$ pli --triple x86_64-unknown-linux-gnu program.pli
error: no embedded runtime for target 'x86_64-unknown-linux-gnu' (this build
embeds: arm64-apple-darwin25.6.0) — rebuild with -DPLI_CROSS_BITCODE=ON, or
compile for the host triple
```

Use `--sysroot <dir>` to link against your own sysroot instead of the
prepared one.

### An architecture pli does not know

For a custom processor (Elbrus e2k and the like) there is no LLVM backend
in this build — but the front end is target-independent, so pli still writes
**portable bitcode** you can finish with your own toolchain:

```
$ pli --triple e2k-unknown-linux-gnu prog.pli -o prog
pli: no LLVM backend for 'e2k-unknown-linux-gnu' in this build: writing portable bitcode
  the runtime symbols stay undefined — build the runtime for your target and link it:
    clang -target e2k-unknown-linux-gnu -O2 -c runtime/rt_*.c
    ar crs libpli_runtime.a rt_*.o
    clang -target e2k-unknown-linux-gnu prog.bc libpli_runtime.a -o prog
```

The bitcode carries no data layout (your toolchain applies the target's own)
and leaves the `pli_*` runtime symbols undefined (you link the runtime you
built). `--emit-llvm` writes the textual form instead — the right one for a
vendor toolchain on an older LLVM, since LLVM bitcode is version-locked.

The full guide is [Cross-compilation](guides/cross-compilation.md).

## Backend plugins

A code-generation backend can be plugged in as a shared object, without
rebuilding the compiler:

```sh
pli --backend-plugin ./mybackend.so --backend acme program.pli -o program
```

The plugin exports `pli_backend_plugin_v1` (see `src/backend/plugin_abi.h`):
it receives the unit's LLVM IR as text and returns the IR to compile. The
compiler re-parses and verifies the result, so a plugin cannot feed invalid
IR into the pipeline. `pli version` lists the loaded plugins.

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
| [guides/cross-compilation.md](guides/cross-compilation.md) | Cross-compilation: targets, sysroots, portable bitcode |
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
| `benchmarks/` | Benchmark programs |
| `scripts/` | Build and code-generation helpers |
| `docs/`, `guides/` | Architecture, ADRs, grammar coverage, user guides |
| `CMakeLists.txt` | Canonical build: LLVM C++ API + lld, embedded runtime bitcode |

## License

Apache 2.0 — see `LICENSE`.
