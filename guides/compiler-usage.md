# Using the pli compiler

This guide is about the `pli-llvm` compiler itself — how to drive it, what flags do, and how to build for another target. It's not about the PL/I language (that's [`guides/programming.md`](programming.md)). Think of `pli` like `gcc` or `clang`: one command parses, optimizes, lowers, and links — all in-process, no external toolchain download needed.

Compile and run any snippet here the usual way:

```bash
pli name.pli -o name
./name
```

> Style note: `.pli` examples use one leading space and modern lowercase, text starting in column 2 and stopping before column 72 (the old card margins). That's the source style `pli` expects; it doesn't affect how you invoke the compiler.

- [Quick reference](#quick-reference)
- [Hello world (compile + run)](#hello-world-compile--run)
- [Output modes](#output-modes)
- [Optimization and checks](#optimization-and-checks)
- [Target and CPU](#target-and-cpu)
- [Cross-compilation](#cross-compilation)
- [Includes and libraries](#includes-and-libraries)
- [C interop and foreign objects](#c-interop-and-foreign-objects)
- [Diagnostics and introspection](#diagnostics-and-introspection)
- [Limitations / not yet](#limitations--not-yet)

---

## Quick reference

| Task                                 | Flag / behavior                                 | Default                                                 |
| ------------------------------------ | ----------------------------------------------- | ------------------------------------------------------- |
| Compile + link to an executable      | `pli file.pli -o out`                           | executable, `-O0`, all checks on                        |
| Object file only                     | `pli file.pli -c`                               | none — opt-in                                           |
| LLVM IR text                         | `pli file.pli --emit-llvm`                      | none — opt-in                                           |
| Keep `.ll` beside output             | `--keep-ll`                                     | off                                                     |
| Shared library                       | `--shared`                                      | executable                                              |
| Static binary                        | `-static`                                       | dynamic                                                 |
| Whole-program link-time optimization | `--flto=thin` / `--full`                        | off                                                     |
| Target triple                        | `--target <t>` / `--triple <t>` / `-target <t>` | host triple                                             |
| Host CPU features                    | `--march=native` (opt-in)                       | `generic`                                               |
| Sysroot override                     | `--sysroot <dir>`                               | embedded multi-OS sysroot                               |
| Include dir                          | `-I <dir>` (repeatable)                         | upward walk from cwd & exe dir                          |
| Library dir / lib                    | `-L <dir>`, `-l<lib>`, `-Wl,<flag>`             | upward walk + `PLI_LIB_PATH`                            |
| Foreign object                       | `.o` / `.obj` linked directly                   | incompatible with `-c` / `--emit-llvm` / `--fsyntax-only` |
| Verbose sub-commands                 | `-v`                                            | quiet                                                   |
| Explain a rule                       | `--explain n` (1–151)                           | —                                                       |
| Version                              | `--version`                                     | —                                                       |

---

## Hello world (compile + run)

A PL/I program needs no imports or headers:

```pli
 hello: procedure options(main);
    put skip list('Hello from pli!');
 end;
```

Save as `hello.pli`:

```bash
pli hello.pli -o hello
./hello
```

`-o hello` names the output. Without `-o`, the compiler picks a target-aware default name in the current directory: `a.out` on non-COFF targets, `a.exe` on COFF (Windows) targets — the name follows the effective triple's object format, not the host OS. On COFF targets an extensionless `-o` name gets `.exe` appended.

> **Python friends:** `put skip list(...)` is `print(...)`.  
> **Go friends:** `procedure options(main)` is `func main()` with a name tag.

---

## Output modes

`pli` is a driver: it parses, lowers, optimizes, and links — all in-process via LLVM + lld, no subprocess toolchain. The output mode decides where the pipeline stops.

### Executable (default)

```bash
pli hello.pli -o hello
```

Link and produce an executable. This is the default — no flag needed.

### Object file only: `-c`

Stop after codegen, emit a relocatable object per input (`<base>.o`, or `<base>.obj` on COFF targets) and do not link:

```bash
pli hello.pli -c -o hello.o
```

Opt-in. You then link `hello.o` yourself or in a later `pli` invocation.

### LLVM IR text: `--emit-llvm` / `--keep-ll`

Dump the LLVM intermediate representation instead of an object:

```bash
pli hello.pli --emit-llvm -o hello.ll
```

Opt-in. Combine with `--keep-ll` to keep the intermediate `.ll` next to the final output:

```bash
pli hello.pli -o hello --keep-ll
```

### Syntax check only: `--fsyntax-only`

Parse and run semantic analysis, but don't emit anything:

```bash
pli hello.pli --fsyntax-only
```

Opt-in — great for CI linting. You can't use it together with `-c`, `--emit-llvm`, `--print-hir`, or `--print-facts`.

### Shared library: `--shared`

Produce a shared library (`.so` / `.dylib` / `.dll`) instead of an executable; all defined external symbols are exported:

```bash
pli mylib.pli --shared -o mylib.so
```

### Statically linked: `-static`

On Linux, link statically where possible:

```bash
pli hello.pli -static -o hello
```

### LTO: `--flto=thin` / `--flto=full`

Whole-program optimization at link time, performed by in-process lld:

```bash
pli hello.pli -O2 --flto=thin -o hello
```

Opt-in; pairs with `-O2` / `-O3`.

---

## Optimization and checks

`-O0` … `-O3` and `-Os` control the optimization level:

```bash
pli hello.pli -O2 -o hello
```

Default is `-O0` (no optimization). `-Os` optimizes for size like `-O3`.

There's also `--release` (maximum optimization + stripped binary) and `--debug` (`-O0`, no DWARF yet). These are presets, not composable knobs — pick one.

### Runtime checks

PL/I traps on things like fixed overflow, subscript range, zero divide, and conversion errors. Each check has an on/off flag:

| Flag | Meaning | Default |
| ---- | ------- | ------- |
| `--size-checks` | force FIXED overflow traps | on at `-O0`/`-O1`; off at `-O2`/`-O3` |
| `--no-size-checks` | elide FIXED overflow traps | off at `-O0`/`-O1`; on at `-O2`/`-O3` |
| `--subscript-checks` | force SUBSCRIPTRANGE checks | on by default |
| `--no-subscript` | elide SUBSCRIPTRANGE checks | off by default |
| `--no-zero-divide` | elide ZERODIVIDE traps | traps on by default |
| `--no-conversion` | elide CONVERSION traps | traps on by default |
| `--no-memory-effects` | elide memory-effect analysis | off |
| `--warn-unset` | warn about possibly-unset variables | off |
| `--remark-library-calls` | remark on library-call opportunities | off |

`-O2` and `-O3` imply `--no-size-checks` (speed over traps). Re-enable with `--size-checks`, which also turns subscript checks back on (the two safety switches travel together — ADR-111).

Example — force traps even at `-O2`:

```bash
pli money.pli -O2 --size-checks -o money
```

And elide them even at `-O0`:

```bash
pli hot.pli -O0 --no-size-checks -o hot
```

> Default vs opt-in: `-O0`/`-O1` ship every check on; `-O2`/`-O3` drop size checks unless you say `--size-checks`.

---

## Target and CPU

### Target triple: `--target` / `--triple` / `-target`

These three spellings are aliases:

```bash
pli hello.pli --target x86_64-unknown-linux-gnu -o hello
pli hello.pli --triple aarch64-unknown-linux-gnu -o hello
pli hello.pli -target x86_64-pc-windows-gnu -o hello
```

Default: the host triple (`llvm::sys::getDefaultTargetTriple()`), so on your machine a plain `pli file.pli -o file` builds for that machine.

Supported cross triples (embedded sysroot + runtime support; musl reuses the Linux runtime blob):

- `x86_64-unknown-linux-gnu` (GNU/Linux, glibc)
- `aarch64-unknown-linux-gnu` (GNU/Linux, glibc)
- `x86_64-unknown-linux-musl` / `aarch64-unknown-linux-musl` (musl Linux)
- `x86_64-pc-windows-gnu` / `aarch64-pc-windows-gnu` (MinGW Windows)
- `x86_64-apple-macosx` / `arm64-apple-macosx` (macOS)

`--target` also decides the object format and linker flavor:

| Triple family | Object format | lld driver | System libs |
| ------------- | ------------- | ---------- | ----------- |
| `*unknown-linux-*` (glibc) | ELF | `elf` | `-lm`, `-lc` |
| `*unknown-linux-musl*` (musl) | ELF | `elf` | reuses the Linux runtime blob |
| `*pc-windows-gnu` | COFF (PE) | `coff` | `kernel32.lib`, `ucrt.lib`, … |
| `*apple-macosx` | Mach-O | `macho` | `-lSystem`, `-lm` |

Linking happens in-process via lld  — no external `ld` or `clang` required.

The embedded sysroot set covers the common targets above (musl targets reuse the Linux runtime blob, per arch + OS family matching). `PLI_CROSS_BITCODE` is the bitcode-generation list and defaults to `OFF`; its targets are:

```
x86_64-unknown-linux-gnu;aarch64-unknown-linux-gnu;
x86_64-pc-windows-gnu;aarch64-pc-windows-gnu
```

### CPU: `--march=native` vs `generic`

Default CPU is `generic` — portable, baseline CI behavior:

```bash
pli hello.pli -o hello          /* generic CPU */
```

`--march=native` opts into the host CPU's feature set. Use it only for local hot runs; it's not reproducible across machines:

```bash
pli hello.pli --march=native -o hello   /* opt-in: host CPU features */
```

---

## Cross-compilation

This is where `pli` differs from a typical compiler: there's no separate clang or target sysroot to download. LLVM, lld, the runtime bitcode, and a multi-OS sysroot are all embedded in the `pli` binary at build time. Pick a triple and go.

### How it works

1. `--target <triple>` selects the LLVM target and object format.
2. IRGen merges the embedded runtime bitcode into the unit. Blob selection matches by arch + OS family: `x86_64-unknown-linux-musl` reuses the Linux runtime blob, for example.
3. The link step uses in-process lld with the matching driver (`elf` / `coff` / `macho`) and the embedded sysroot for that arch+OS, which provides the C library headers/libraries (or macOS `.tbd` stubs) needed to link.

That last point matters: **a cross link needs a sysroot with libc for that target**. The embedded set covers the common targets above but not arbitrary target libraries — don't over-promise cross-compile to an unsupported triple.

### `--sysroot <dir>`

`--sysroot` replaces the embedded sysroot entirely with a directory you provide:

```bash
pli hello.pli --target aarch64-unknown-linux-gnu \
  --sysroot /opt/sysroots/aarch64-linux -o hello
```

The directory must exist (the compiler checks and fails otherwise). User-provided sysroots are never deleted; only compiler-owned temporary embedded dirs are cleaned up.

Layout: arch-prefixed logical paths. Under your sysroot, `pli` looks for target libraries keyed by the triple's arch and OS family. Missing files under the sysroot fail the link with **"incomplete sysroot"** — that's a signal to point `--sysroot` at a complete tree, not to fall back silently.

### Worked examples

From any host (Linux, macOS, or Windows):

```bash
# Linux-musl aarch64 from an x86-64 Linux host
pli hello.pli --target aarch64-unknown-linux-musl -o hello

# MinGW x86_64 Windows from any host
pli hello.pli --target x86_64-pc-windows-gnu -o hello.exe

# macOS arm64 from any host
pli hello.pli --target arm64-apple-macosx -o hello

# Your own complete sysroot
pli hello.pli --target x86_64-unknown-linux-gnu \
  --sysroot $HOME/sysroots/x86_64 -o hello
```

Darwin: runtime bitcode is vendored under `platform/darwin-runtime/{arm64,x86_64}/`, and blob selectors refuse the host-blob fallback for Darwin — a Darwin target always uses its own vendored blob.

### Build note: `PLI_CROSS_BITCODE`

The embedded per-target bitcode is generated only when `PLI_CROSS_BITCODE=ON` at CMake configure time; it defaults to `OFF`. Targets in `PLI_TARGETS`:

```
x86_64-unknown-linux-gnu;aarch64-unknown-linux-gnu;
x86_64-pc-windows-gnu;aarch64-pc-windows-gnu
```

If you build `pli` yourself and need cross targets, configure with `-DPLI_CROSS_BITCODE=ON`. If you download a release binary, cross bitcode comes with it.

### "incomplete sysroot"

When a file the linker needs isn't found under the selected sysroot (embedded or `--sysroot`), lld fails the link with **"incomplete sysroot"**. Fix: supply a complete sysroot with `--sysroot`, or confirm the target triple is one of the embedded ones.

---

## Includes and libraries

### `%INCLUDE` search: `-I`

`-I` is repeatable (`-I inc -I ../common`), and `-I<dir>` (no space) works too:

```bash
pli main.pli -I inc -I../common -o main
```

Search order for `%include` / `%xinclude`:

1. the including file's directory,
2. repeatable `-I` flags (first match wins),
3. `PLI_INCLUDE_PATH` (colon-separated, like `CPATH`),
4. `include/` / `inc/` found by walking up from the source (cwd) and from the `pli` executable,
5. static system paths (`/usr/include`, `/usr/local/include` and their `pli` subdirs) on Linux/macOS,
6. executable-relative `share/pli/include`.

`PLI_LIB_PATH` (colon-separated, like `LIBRARY_PATH`) is searched after user `-L` and before the upward-walk and static system library dirs.

### Link libraries: `-L` / `-l` / `-Wl,`

```bash
pli main.pli -L /opt/lib -lm -Wl,--allow-multiple-definition -o main
```

`-L` adds a library search path, `-l<lib>` links a library, `-Wl,<flag>` passes a raw flag to the linker. `-static` produces a statically linked binary.

Use `-v` to list the configured search paths:

```bash
pli hello.pli -v -o hello
```

---

## C interop and foreign objects

Foreign `.o` / `.obj` inputs (from C or another language) are passed straight to the link step — never preprocessed. Only `.pli` / `.inc` are PL/I sources:

```bash
pli main.pli helper.o -o main
```

Constraint: object inputs can't be combined with `-c`, `--emit-llvm`, `--emit-mlir`, `--fsyntax-only`, `--print-hir`, or `--print-facts`. Those modes need PL/I source to work on, so the compiler errors out instead of silently dropping the object.

See [C interoperability](programming.md#c-interoperability) for the PL/I side of calling C (`z char(20) varyingz`, `based` overlays, etc.).

---

## Diagnostics and introspection

### Rule-numbered diagnostics

PL/I has no reserved words and no silent gaps. A feature the compiler doesn't implement is diagnosed with a TR 25.084 rule number — like `(24)` or `(91)` — never silently accepted.

### `--explain n`

Print rule `n`'s production (1–151) and exit:

```bash
pli --explain 24
```

### Verbose: `-v`

Show the sub-commands being run (parse, lower, optimize, link). Helpful for debugging search paths and the link step:

```bash
pli hello.pli -v -o hello
```

### Introspection flags

| Flag | What it prints |
| ---- | -------------- |
| `--print-hir` | the HIR after semantic analysis |
| `--print-facts` | collected range/type facts |
| `--remark-library-calls` | library-call opportunities |
| `--no-range-facts` | disable range facts |
| `--version` | pli + LLVM versions, host triple, embedded runtime/sysroot sizes |
| `--sysparm` | value returned by the SYSPARM builtin (rule (123)); also `$PLI_SYSPARM` env |

`--version` needs no input file:

```bash
pli --version
```

---

## Limitations / not yet

- No separate clang download: target selection is in-process LLVM; cross link needs a sysroot with libc for that target unless the embedded set covers it.
- The embedded set covers common targets (Linux glibc/musl, MinGW Windows, macOS) but not arbitrary target libraries.
- `PLI_CROSS_BITCODE` defaults OFF — rebuild with it ON if you compile `pli` from source and need cross bitcode.
- `--subsystem console|windows` is only valid for COFF (Windows) targets.
- Object inputs are incompatible with `-c` / `--emit-llvm` / `--fsyntax-only` / `--print-hir` / `--print-facts`.

---

> Companion guide: [`guides/programming.md`](programming.md) covers the PL/I language itself — data types, procedures, conditions, I/O, and built-ins.
