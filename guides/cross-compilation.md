# Cross-compilation

`pli --triple <t>` compiles for another target. This guide covers the three
output modes, how to get a target's sysroot, and what to do when pli knows
nothing about your architecture.

## The three modes

Every target has a **profile**: what this build has for it — an LLVM backend,
an embedded runtime, a link sysroot. The profile picks the output mode:

| Mode | You get | You supply |
| --- | --- | --- |
| **self-contained** | an object or an executable | nothing (the target's sysroot is embedded or prepared) |
| **object + external runtime** | an `.o` with undefined `pli_*` symbols | a runtime for the target, and your linker |
| **portable bitcode** | a `.bc` (or `.ll`) with no codegen | your toolchain and your runtime |

The mode is chosen automatically; you rarely pick it yourself. The two
switches that matter:

- `--emit-bitcode` — write portable bitcode for *any* target (also useful
  with a target pli does know, e.g. to LTO with your own toolchain).
- `--no-portable` — fail instead of falling back to portable bitcode when
  the target has no LLVM backend.

## The targets this build knows

`pli version` lists them:

```
$ pli version
pli ... (LLVM 23.1.2, arm64-apple-darwin25.6.0, runtime 175152 bytes, ...)
cross targets: arm64-apple-darwin25.6.0 x86_64-unknown-linux-gnu aarch64-apple-macosx15.0
```

| Target | Needs |
| --- | --- |
| `x86_64-unknown-linux-gnu` | a musl sysroot |
| `aarch64-unknown-linux-gnu` | a musl sysroot |
| `x86_64-pc-windows-gnu` | a mingw-w64 sysroot |
| `aarch64-apple-macosx15.0` | the macOS SDK of the build host |

`aarch64-pc-windows-gnu` is **not** supported: mingw-w64 has no aarch64 port,
so no sysroot exists for it. A target that is not in the list is not an
error — see *An architecture pli does not know* below.

## Prepare a sysroot

A cross target needs the target's C library headers (to build the runtime)
and its CRT objects and libraries (to link). The script builds both in a
container and installs them in a local cache:

```sh
sh scripts/prepare_cross_sysroot.sh x86_64-unknown-linux-gnu
sh scripts/prepare_cross_sysroot.sh aarch64-unknown-linux-gnu
sh scripts/prepare_cross_sysroot.sh x86_64-pc-windows-gnu
```

It needs Docker; the sysroots land in `~/.pli/sysroots/<target>` (override
with `PLI_SYSROOT_DIR`). Without Docker, `--source download` fetches a
prebuilt toolchain instead:

```sh
sh scripts/prepare_cross_sysroot.sh x86_64-unknown-linux-gnu --source download
```

A macOS target needs no preparation: the SDK of the build host is used, so
only a macOS build host can target macOS.

## Build and use

```sh
cmake -G Ninja -S . -B build/cmake -DPLI_CROSS_BITCODE=ON   # plus platform flags
cmake --build build/cmake -j

./build/cmake/pli --triple x86_64-unknown-linux-gnu program.pli -o program
./build/cmake/pli --triple x86_64-pc-windows-gnu program.pli -o program.exe
./build/cmake/pli --triple aarch64-unknown-linux-gnu program.pli -c -o program.o
```

A target without a runtime bitcode is diagnosed, never served with the
host's:

```
$ pli --triple x86_64-unknown-linux-gnu program.pli
error: no embedded runtime for target 'x86_64-unknown-linux-gnu' (this build
embeds: arm64-apple-darwin25.6.0) — rebuild with -DPLI_CROSS_BITCODE=ON, or
use --no-bitcode-runtime (an object with external runtime references) /
--emit-bitcode (portable bitcode), or compile for the host triple
```

Use `--sysroot <dir>` to link against your own sysroot instead of the
prepared one.

## An architecture pli does not know

For a custom processor (Elbrus e2k and the like) there is no LLVM backend in
this build — but the front end is target-independent, so pli still produces
**portable bitcode** you can finish with your own toolchain:

```
$ pli --triple e2k-unknown-linux-gnu prog.pli -o prog
pli: no LLVM backend for 'e2k-unknown-linux-gnu' in this build: writing portable bitcode
  the runtime symbols stay undefined — build the runtime for your target and link it:
    clang -target e2k-unknown-linux-gnu -O2 -c runtime/rt_*.c
    ar crs libpli_runtime.a rt_*.o
    clang -target e2k-unknown-linux-gnu prog.bc libpli_runtime.a -o prog
  (use --emit-llvm for textual IR; see guides/cross-compilation.md)
```

`prog.bc` is the portable artifact. Its contract:

- **no data layout** — your toolchain applies the target's own;
- the **target triple** is kept, so the consumer knows what it was built for;
- the **`pli_*` runtime symbols are undefined** — you link the runtime;
- it is **not optimized** (the pipeline needs a target machine) — your
  toolchain optimizes it.

The recipe, step by step:

```sh
# 1. the runtime for your target (the TUs are exactly runtime/rt_*.c)
sh scripts/build_runtime_for.sh e2k-unknown-linux-gnu
#    (add --sysroot DIR when your toolchain needs the headers)

# 2. compile the portable bitcode with your toolchain
clang -target e2k-unknown-linux-gnu -c prog.bc -o prog.o

# 3. link it with the runtime
clang -target e2k-unknown-linux-gnu prog.o libpli_runtime.a -o prog
```

If your OS' APIs differ from the default platform TU, write one against
`runtime/pli_rt_plat.h` and pass it with `--rt-platform`.

### The ABI assumption

The module is built with a **neutral LP64 layout** (an unknown architecture is
assumed 64-bit). If your ABI is not LP64 — ILP32, or a different pointer
width — state it:

```sh
pli --triple e2k-unknown-linux-gnu --data-layout "e-m:o-p:32:32-i64:64-i128:128-n32:64-S128" \
    prog.pli -o prog
```

### Bitcode or text?

LLVM **bitcode is version-locked**: a newer LLVM reads older bitcode, but an
older LLVM cannot read newer bitcode. A vendor toolchain on an older LLVM
needs the textual form:

| Your toolchain's LLVM | Use |
| --- | --- |
| the same version as pli's, or newer | `.bc` (`--emit-bitcode`) |
| older (a vendor fork) | `.ll` (`--emit-llvm`) |

The IR *text* drifts less than the bitcode, but it still drifts between
major versions: check that your LLVM parses the emitted IR.

## The runtime is C, not PL/I

The `pli_*` runtime is plain C (`runtime/*.c`), which is what makes the
portable flow work: you compile it with *your* compiler, for *your* target.
The PL/I side and the runtime agree on the ABI because both are built from
`runtime/pli_rt.h`.

The runtime TUs are exactly `runtime/rt_*.c` — the portable ones plus the
platform TU (`rt_plat_default.c`). A custom platform replaces the last one.

## See also

- ADR-234 — the cross-target model (ABI families, per-target runtime and
  sysroot).
- ADR-236 — portable bitcode for unsupported architectures.
- `docs/ARCHITECTURE.md` — the pipeline and the target profile.
