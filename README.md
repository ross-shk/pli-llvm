# plic — a PL/I compiler targeting LLVM

A modern PL/I compiler built to the formal specification: **TR 25.084** (Concrete Syntax) and **Y33-6003** (Semantics). The extracted grammar lives in `TR25.084-concrete-syntax.md`.

**Single-binary distribution** (like Go/Zig) — the `plic` executable embeds the PL/I runtime bitcode (`libpli.bc`) and links via in-process `lld`. No installer, no separate runtime to deploy.

```bash
$ cmake -G Ninja -S . -B build/cmake -DCMAKE_PREFIX_PATH=$(brew --prefix llvm)
$ cmake --build build/cmake -j$(nproc)
$ ./build/cmake/plic tests/core/hello.pli -o hello
$ ./hello
Hello, world!
```

### Platforms
- **Linux** (x86_64, ARM64): clang or gcc
- **macOS** (Intel, Apple Silicon): clang
- **Windows** (x86_64, ARM64): **MSVC/nmake** (not MinGW); portable threading abstraction wraps POSIX pthreads and Win32 primitives

---

## Quick Start

### Prerequisites
- C++20 compiler (clang++/g++; MSVC on Windows)
- CMake ≥ 3.20
- Ninja build system (`brew install ninja` / `apt-get install ninja-build`)
- LLVM ≥ 18 with `clang` and `lld`

### Build
```bash
# macOS (Homebrew LLVM):
cmake -G Ninja -S . -B build/cmake -DCMAKE_PREFIX_PATH=$(brew --prefix llvm)
cmake --build build/cmake -j$(nproc)

# Linux:
cmake -G Ninja -S . -B build/cmake
cmake --build build/cmake -j$(nproc)
```

### Cross-compile PL/I code
Compile for another target with `--triple` (build `plic` with `PLIC_CROSS_BITCODE=ON` first):
```bash
./build/plic --triple x86_64-unknown-linux-gnu program.pli -o program
./build/plic -target aarch64-pc-windows-gnu program.pli -o program.exe
```

### Run a PL/I program
```pli
hello: procedure options(main);
    put skip list('Hello, world!');
end hello;
```

```bash
./build/plic tests/core/hello.pli -o hello
./hello
```

### Test
```bash
ctest --test-dir build/cmake     # ~440 tests
./tests/run_tests.py usecases   # specific group
```

---

## What Works Today

| Feature | Examples |
|---------|----------|
| **Procedures & functions** | `OPTIONS(MAIN)`, internal, `RETURNS`, `RECURSIVE`, multiple entry points |
| **Static link for nested scopes** | Internal procedures access enclosing automatics; externals stay reentrant |
| **C interop** | `ENTRY ... EXTERNAL`, `BYVALUE`, `LINKAGE(SYSTEM)` for scalars/pointers |
| **Data types** | `FIXED BIN/DEC(p,q)`, `FLOAT`, `COMPLEX`, `BIT`, `CHAR/VARYING/VARYINGZ`, `POINTER` |
| **Arrays & structures** | Dynamic extents, cross-sections `A(i,*)`, reductions `SUM`/`PROD`/`ANY`/`ALL`, `LIKE`, `BY NAME` |
| **Storage** | `BASED`/`ALLOCATE/FREE`, `CONTROLLED` stacks, `DEFINED` with `iSUB`, `ADDR`/`NULL` |
| **Conditions** | `ON`/`SIGNAL`/`REVERT` for `SIZE`, `SUBSCRIPTRANGE`, `ZERODIVIDE`, named conditions |
| **Concurrency** | `TASK`/`EVENT`/`PRIORITY` async `CALL` with `WAIT`/`DELAY` |
| **Stream I/O** | `PUT`/`GET LIST`/`EDIT`/`DATA`, `FILE`/`STRING` routing, `OPEN`/`CLOSE` |
| **Record I/O** | `WRITE`/`READ` fixed-size binary records |
| **Preprocessor** | `%INCLUDE`, `%DECLARE`, `%IF/%THEN/%ELSE`, `%ACTIVATE`/`%DEACTIVATE` |
| **Built-ins** | String, math, array/pointer/misc (see `GRAMMAR-COVERAGE.md`) |
| **No reserved words** | `IF`, `THEN`, `ELSE`, `DO`, `END`, `PUT` are ordinary variables |

Everything else is diagnosed with its TR 25.084 rule number — the diagnostic *is* the to-do list.

---

## Interesting Example: Prime Sieve

```pli
sieve: procedure options(main);
    declare (n, i, k, count, limit) fixed bin(31);
    declare primes(1000) fixed bin(31);
    declare sieve(1000) bit(1);

    put skip list('Limit:');
    get list(n);
    limit = min(n, 1000);

    sieve = '1'b;
    sieve(1) = '0'b;
    count = 0;

    do i = 2 to limit;
        if sieve(i) then do;
            count = count + 1;
            primes(count) = i;
            do k = i * i to limit by i;
                sieve(k) = '0'b;
            end;
        end;
    end;

    put skip list('Found ');
    put skip list(count);
    put skip list(' primes');
    put skip list('First 10:');
    do i = 1 to min(10, count);
        put skip list(primes(i));
    end;

    put skip list('PASS');
end sieve;
```

Shows: arrays, `BIT` arrays, dynamic extents, `DO` loops with `TO/BY`, arithmetic, `MIN`, string handling, and list-directed I/O.

---

## Documentation

| Doc | Purpose |
|-----|---------|
| [CONTRIBUTING.md](CONTRIBUTING.md) | How to add features: layer map, workflow, invariants |
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | Pipeline, IR levels, data representation, ABI, runtime |
| [GRAMMAR-COVERAGE.md](docs/GRAMMAR-COVERAGE.md) | Rule-by-rule implementation status |
| [SPEC-COMPLIANCE-REPORT.md](docs/SPEC-COMPLIANCE-REPORT.md) | TR 25.084 / Y33-6003 audit: GAP-ANALYSIS, CONFORMANCE-MATRIX, REMEDIATION-PLAN |
| [DESIGN-DECISIONS.md](docs/DESIGN-DECISIONS.md) | Historical design decisions (ADRs) |

---

## Layout

```
src/         compiler: diag, lexer, parser, sema, hir, irgen, preprocessor, driver
runtime/     libpli: core, stream, string, math, conditions, storage, task
tests/       8 groups: core, builtins, usecases, driver, ir, preprocessor, multimodule, corner_cases
docs/        architecture, decisions, optimization, plans, coverage
```

---

## Building & Testing

```bash
cmake --build build/cmake -j$(nproc)    # build via CMake/Ninja
ctest --test-dir build/cmake           # test suite
cmake --build build/cmake --target check   # quality gate (fmt + tidy + werror)
cmake --build build/cmake --target clean
```

CMake directly:
```bash
cmake -G Ninja -S . -B build/cmake -DCMAKE_PREFIX_PATH=$(brew --prefix llvm)
cmake --build build/cmake -j$(nproc)
ctest --test-dir build/cmake
```

Windows: use MSVC + CMake (no MinGW/MSYS2). The threading abstraction in `runtime/sync/plic_thread.h` wraps `pthread_once`/`pthread_mutex`/`pthread_cond` and `InitOnceExecuteOnce`/`CRITICAL_SECTION`/`CONDITION_VARIABLE`/`_beginthreadex`/`Sleep`.

---

## Cross-Compilation

`plic` can target Linux x86_64, Linux ARM64, Windows x86_64, Windows ARM64 from any host (requires LLVM with the target backends and `lld`).

`plic` is built natively on each host — no cross-compiling the compiler binary.
`--triple`/`-target` cross-compiles **user PL/I code** using LLVM with the
matching target backend and `lld`.

**Build with multi-target runtime bitcode** (embeds all 4 targets; requires
`PLIC_CROSS_BITCODE=ON` at build time):
```bash
cmake -G Ninja -S . -B build/cmake -DCMAKE_PREFIX_PATH=$(brew --prefix llvm) -DPLIC_CROSS_BITCODE=ON
cmake --build build/cmake -j$(nproc)
```

**Compile for a target**:
```bash
./build/plic --triple x86_64-unknown-linux-gnu program.pli -o program
./build/plic -target aarch64-pc-windows-gnu program.pli -o program.exe
```

Without `PLIC_CROSS_BITCODE`, the per-target runtime blobs are empty stubs;
`--triple` then falls back to the host runtime blob, so cross-compilation of
user code works for targets whose ABI matches the host (e.g. macOS-arm64 →
Linux-arm64). Build with `PLIC_CROSS_BITCODE=ON` for genuine multi-target
runtime bitcode across all 4 targets.

---

## Known Deviations

1. **`/` and `**` use floating-point** (ADR-014); exact `FIXED` division/scale is D1/QR2
2. **Unimplemented = diagnosed** with rule number — see `GRAMMAR-COVERAGE.md`

---

## License

MIT — see `LICENSE`.