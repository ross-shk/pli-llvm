# plic — a PL/I compiler targeting LLVM

A modern PL/I compiler built to the formal specification: **TR 25.084** (Concrete Syntax) and **Y33-6003** (Semantics).

**Single-binary distribution** (like Go/Zig) — `plic` embeds the PL/I runtime bitcode (`libpli.bc`) and links via in-process `lld`. No installer, no separate runtime to deploy.

```pli
hello: procedure options(main);
    put skip list('Hello, world!');
end hello;
```

```sh
plic tests/core/hello.pli -o hello
./hello
```

A more practical taste — parallel sum with two tasks sharing a heap array. `ALLOCATE ... SET` takes a heap block, each `CALL ... EVENT` runs on its own thread, `WAIT` joins, and `FREE` releases the block:

```pli
parsum: procedure options(main);
    declare i fixed bin(31);
    declare heap pointer;
    declare data(100) fixed bin(31) based(heap);
    declare ev1 event;
    declare ev2 event;
    declare part1 fixed bin(31);
    declare part2 fixed bin(31);

    allocate data set(heap);
    do i = 1 to 100;
        data(i) = i;
    end;

    part1 = 0; part2 = 0;
    call sum_first event(ev1);
    call sum_second event(ev2);
    wait(ev1, ev2);

    put skip list('sum 1..100 =', part1 + part2);
    free data;

sum_first: procedure;
    declare j fixed bin(31);
    declare s fixed bin(31);
    s = 0;
    do j = 1 to 50;
        s = s + data(j);
    end;
    part1 = s;
end sum_first;

sum_second: procedure;
    declare j fixed bin(31);
    declare s fixed bin(31);
    s = 0;
    do j = 51 to 100;
        s = s + data(j);
    end;
    part2 = s;
end sum_second;

end parsum;
```

```sh
plic parsum.pli -o parsum
./parsum
sum 1..100 =        5050
```

## Platforms

- **Linux** (x86_64, ARM64): gcc or clang
- **macOS** (Intel, Apple Silicon): clang
- **Windows** (x86_64, ARM64): MSVC + Ninja (not MinGW/MSYS2/Cygwin)

Threading is abstracted in `runtime/sync/plic_thread.h` (POSIX pthreads on Linux/macOS, Win32 primitives on Windows).

---

## Requirements

| Tool               | Minimum                            | Notes                                                        |
| ------------------ | ---------------------------------- | ------------------------------------------------------------ |
| C++20 compiler     | gcc 12 / clang 15 / MSVC 2022 17.x | Plus a Windows 10/11 SDK on Windows                          |
| CMake              | 3.20                               | All platforms                                                |
| Ninja              | any recent                         | Canonical generator on all platforms, including Windows      |
| LLVM + clang + lld | 18                                 | Dev libraries **and** `lld` headers/libs required; see below |
| Python             | 3                                  | For `scripts/gen_*.py` and `tests/run_tests.py`              |
| Git                | any recent                         | Optional; without it `plic version` reports `unknown`        |

LLVM sources:

- **Linux:** distro `llvm`/`clang`/`lld` dev packages, or a self-built LLVM.
- **macOS:** `brew install llvm lld ninja cmake` (note: stock `llvm` formula omits `lld`, so `lld` is a separate formula).
- **Windows:** no standard prebuilt LLVM ships everything `plic` needs, so build LLVM 18.x from source with `clang;lld` and the `X86;AArch64` targets (one-time cost), then point `CMAKE_PREFIX_PATH` at the install dir. Forward slashes work best in CMake paths (`C:/llvm-install`).

---

## Build

### Linux

```sh
cmake -G Ninja -S . -B build/cmake
cmake --build build/cmake -j
```

### macOS (Homebrew LLVM)

```sh
cmake -G Ninja -S . -B build/cmake -DCMAKE_PREFIX_PATH="$(brew --prefix llvm)"
cmake --build build/cmake -j
```

### Windows (MSVC + Ninja)

Run from `cmd.exe` (not PowerShell, not MinGW):

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64

cmake -G Ninja -S C:/path/to/pli-llvm -B C:/path/to/pli-llvm/build/cmake ^
  -DCMAKE_PREFIX_PATH=C:/path/to/llvm-install ^
  -DCMAKE_BUILD_TYPE=Release

cmake --build C:/path/to/pli-llvm/build/cmake -j
```

Notes:

- `vcvarsall.bat x64` is required so MSVC, the Windows SDK, and `cl.exe` are on `PATH`.
- `CMAKE_PREFIX_PATH` must point at your LLVM **install** prefix (the directory containing `lib/cmake/llvm`). Use forward slashes.
- Ninja must be on `PATH` (`pip install ninja`, or a Ninja release, or the VS-bundled copy).
- Do not use MinGW, MSYS2, Cygwin, or WSL `cmake`/`ninja` to build `plic` itself — use native MSVC.

Building LLVM 18.x on Windows (one-time setup, Release, `X86;AArch64` only):

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64

cmake -G Ninja -S llvm-project/llvm -B llvm-build ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DLLVM_ENABLE_PROJECTS="clang;lld" ^
  -DLLVM_TARGETS_TO_BUILD="X86;AArch64" ^
  -DCMAKE_INSTALL_PREFIX=C:/llvm-install

cmake --build llvm-build -j --target install
```

Then build `plic` with `-DCMAKE_PREFIX_PATH=C:/llvm-install`.

### Test

```sh
ctest --test-dir build/cmake -j        # full suite (~450 tests)
python3 tests/run_tests.py usecases    # one group
```

Quality gate (format + static analysis + warnings-as-errors):

```sh
cmake --build build/cmake --target check
```

---

## Use

After build, make sure `plic`from `./build/cmake/plic` is in the executable path, compile for the host:

```sh
plic tests/core/hello.pli -o hello
./hello
```

Cross-compile PL/I code with `--triple` (first build `plic` with `PLIC_CROSS_BITCODE=ON` so all 4 runtime bitcodes are embedded):

```sh
cmake -G Ninja -S . -B build/cmake -DPLIC_CROSS_BITCODE=ON   # + platform flags above
cmake --build build/cmake -j

./build/cmake/plic --triple x86_64-unknown-linux-gnu program.pli -o program
./build/cmake/plic --triple x86_64-pc-windows-gnu program.pli -o program.exe
```

Supported triples: `x86_64-unknown-linux-gnu`, `aarch64-unknown-linux-gnu`, `x86_64-pc-windows-gnu`, `aarch64-pc-windows-gnu`. Without `PLIC_CROSS_BITCODE=ON`, `--triple` falls back to the host runtime blob and only works when the target ABI matches the host.

---

## What works today

| Feature                           | Examples                                                                                                                                                                                                                                                                  |
| --------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Procedures & functions**        | `OPTIONS(MAIN)`, internal, `RETURNS`, `RECURSIVE`, `ENTRY` alternate entries, `PACKAGE`/`EXPORTS`                                                                                                                                                                         |
| **Static link for nested scopes** | Internal procedures access enclosing automatics; externals stay reentrant                                                                                                                                                                                                 |
| **C interop**                     | `ENTRY ... EXTERNAL`, `BYVALUE`, `BYADDR`, `LINKAGE(SYSTEM)`, `OPTIONAL`/`*` + `OMITTED`/`PRESENT`, `ENTRY VARIABLE` callbacks, `UNALIGNED` packed structs, `SYSTEM()`                                                                                                    |
| **Data types**                    | `FIXED BIN/DEC(p,q)`, `FLOAT`, `COMPLEX`, `BIT(n)` packed, `CHAR/VARYING/VARYINGZ`, `POINTER`, `AREA`/`OFFSET`, `VALUE` constants, `DEFINE ALIAS`/`TYPE`                                                                                                                  |
| **Arrays & structures**           | Dynamic/adjustable (`*`) extents, negative bounds, cross-sections `A(i,*)`, slice assignment, whole-array expressions, reductions `SUM`/`PROD`/`ANY`/`ALL`, arrays of structures, `LIKE`, `BY NAME`                                                                       |
| **Storage**                       | `BASED`/`ALLOCATE/FREE` (+`IN(area)`), `CONTROLLED` stacks, `DEFINED` with `iSUB`, `ADDR`/`NULL`                                                                                                                                                                          |
| **Conditions**                    | `ON`/`SIGNAL`/`REVERT` for `ERROR`, `SIZE`, `SUBSCRIPTRANGE`, `ZERODIVIDE`, `CONVERSION`, named conditions; `(NO...)` prefixes, `ONCODE()`, nested `ON`                                                                                                                   |
| **Concurrency**                   | `TASK`/`EVENT`/`PRIORITY` async `CALL` with `WAIT`/`DELAY`, `EVENT()` poll                                                                                                                                                                                                |
| **Control flow (extensions)**     | `SELECT`/`WHEN`/`OTHERWISE`, `LEAVE`/`ITERATE`, `DO UNTIL`, local `GO TO`, `//` comments                                                                                                                                                                                  |
| **Stream I/O**                    | `PUT`/`GET LIST`/`EDIT`/`DATA`, `FILE`/`STRING` routing, `OPEN`/`CLOSE`, `FORMAT` + `R(label)`, `DISPLAY`                                                                                                                                                                 |
| **Record I/O**                    | `WRITE`/`READ` sequential fixed-size binary records                                                                                                                                                                                                                       |
| **Preprocessor**                  | `%INCLUDE`/`%XINCLUDE` (+`-I`, `PLIC_INCLUDE_PATH`), `%DECLARE`, `%IF/%THEN/%ELSE`, `%ACTIVATE`/`%DEACTIVATE`, `%REPLACE`                                                                                                                                                 |
| **Separate compilation & driver** | `EXTERNAL` linkage across units, multi-unit `plic` invocation, `-c`/`-emit-llvm`, `-I`, `--sysparm`, `-v`                                                                                                                                                                 |
| **Built-ins**                     | String (+`TRIM`/`TALLY`/`UPPERCASE`/`LOWERCASE`/`CENTER`/`SEARCH`/`RANK`/`COLLATE`/`HIGH`/`LOW`/`DATE`/`TIME`), `CHAR`/`FIXED`, math (+degree trig, `ATAN2`/`CBRT`/`ERF`), `COMPLEX`/`REAL`/`IMAG`/`CONJG`, `LBOUND`/`HBOUND`/`DIM`, `SYSPARM` (see `guides/builtins.md`) |
| **No reserved words**             | `IF`, `THEN`, `ELSE`, `DO`, `END`, `PUT` are ordinary variables                                                                                                                                                                                                           |

Everything else is diagnosed with its TR 25.084 rule number — the diagnostic *is* the to-do list. Larger examples live in `tests/usecases/` and `benchmarks/`.

---

## Documentation

| Doc                                                         | Purpose                                                                  |
| ----------------------------------------------------------- | ------------------------------------------------------------------------ |
| [CONTRIBUTING.md](CONTRIBUTING.md)                          | How to add features: layer map, workflow, invariants                     |
| [guides/programming.md](guides/programming.md)              | Feature guide: data types, arrays, storage, I/O, conditions, concurrency |
| [guides/builtins.md](guides/builtins.md)                    | Built-in functions reference                                             |
| [ARCHITECTURE.md](docs/ARCHITECTURE.md)                     | Pipeline, IR levels, data representation, ABI, runtime                   |
| [GRAMMAR-COVERAGE.md](docs/GRAMMAR-COVERAGE.md)             | Rule-by-rule implementation status                                       |
| [SPEC-COMPLIANCE-REPORT.md](docs/SPEC-COMPLIANCE-REPORT.md) | TR 25.084 / Y33-6003 audit                                               |
| [DESIGN-DECISIONS.md](docs/DESIGN-DECISIONS.md)             | Historical design decisions (ADRs)                                       |

## Layout

| Path                          | Contents                                                                                                                |
| ----------------------------- | ----------------------------------------------------------------------------------------------------------------------- |
| `src/`                        | Compiler: lexer, parser, sema, HIR, IRGen, preprocessor, diagnostics, target, codegen, embedded runtime                 |
| `runtime/`                    | `libpli` (I/O, strings, conditions, storage, math, tasks) + `sync/plic_thread.h` threading abstraction                  |
| `tests/`                      | `run_tests.py` + groups (`core`, `builtins`, `usecases`, `driver`, `ir`, `preprocessor`, `multimodule`, `corner_cases`) |
| `CMakeLists.txt`              | Canonical build: LLVM C++ API + `lld`, embedded runtime bitcode                                                         |
| `docs/` / `guides/`           | Architecture, ADRs, grammar coverage, compliance report, programming guides                                             |
| `TR25.084-concrete-syntax.md` | The spec: rules (1)–(151)                                                                                               |

---

## Known deviations

1. **`/` and `**` use floating-point** (ADR-014); exact `FIXED` division/scale is D1/QR2.
2. **Unimplemented = diagnosed** with rule number — see `GRAMMAR-COVERAGE.md`.

---

## License

MIT — see `LICENSE`.
