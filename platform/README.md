# Vendored link sysroots (ADR-226, ADR-227)

Embed sources for the link sysroot, so user programs build with nothing
except the OS installed — no Xcode, no build-essential, no MSVC. These dirs
are COMPILED INTO `pli(.exe)` at build time
(`scripts/gen_sysroot_embed.py`, one TU per file) and materialized to a temp
dir at link time — release archives hold only the binary, never these dirs.
An exe-relative `musl/`/`mingw/`/`sdk/` dir next to the binary overrides the
embedded set (dev use: iterate on sysroot files without rebuilding).

| Dir | OS | Embedded contents | Provenance |
| --- | -- | -------- | ---------- |
| `darwin-sdk/` | macOS | hand-written `.tbd` stubs (linker symbol names only): `usr/lib/libm.tbd`, `usr/lib/libSystem.tbd` | ADR-226 |
| `linux-musl/x86_64/`, `linux-musl/aarch64/` | Linux | musl static sysroot: `crt1.o`, `crti.o`, `crtn.o`, `libc.a`, `libpli_builtins-<arch>.a` (compiler-rt 128-bit helpers renamed — see below), empty `libm.a`/`libpthread.a`/`libdl.a`/`librt.a` stubs so user `-l` flags resolve | x86_64: Debian `musl-tools` 1.2.4 (`/usr/lib/x86_64-linux-musl/`) + LLVM 23 resource dir (`lib/clang/23/lib/linux/libclang_rt.builtins-<arch>.a`, renamed); aarch64: Ubuntu `musl-dev` 1.2.4-2 (`/usr/lib/aarch64-linux-musl/`) + Ubuntu `libclang-rt-22-dev` 22.1.8-1ubuntu1 (`/usr/lib/llvm-22/lib/clang/22/lib/linux/libclang_rt.builtins-aarch64.a`, renamed) |
| `windows-mingw/` | Windows | mingw-w64 sysroot: `crt2.o`, `crtbegin.o`, `crtend.o`, `fltused.o` (built from `fltused.c` in this dir), `chkstk.o` (built from `chkstk.s`: `__chkstk` → `___chkstk_ms` tail-call for dynamic-alloca stack probes), `libgcc.a`, `libkernel32.a`, `libmsvcrt.a`, `libmingw32.a`, `libmingwex.a`, `libmoldname.a`, `libucrt.a`, `libadvapi32.a`, `libshell32.a`, `libuser32.a` (`fltused.c`/`chkstk.s` are rebuild sources, not link inputs) | Debian `mingw-w64` 11.0.1 + `g++-mingw-w64` 13-win32 (`/usr/x86_64-w64-mingw32/lib/`, `/usr/lib/gcc/x86_64-w64-mingw32/13-win32/libgcc.a`) |

`libgcc.a` ships under the GCC Runtime Library Exception (redistributable
with programs); musl is MIT; mingw-w64 runtime is public domain/Zope.
Per-arch releases carry the matching arch's files from
`platform/linux-musl/<arch>/`; the build embeds only the matching arch and
warns when the arch is missing.

## Prepared cross sysroots (ADR-234)

The dirs above are the **host** link sysroots: they are embedded in the
binary and make a host link need nothing but the OS. A *cross* target needs
its own sysroot, and the target's headers are needed too (to compile the
PL/I runtime for that target), which the dirs above do not carry.

`scripts/prepare_cross_sysroot.sh <target>` builds one in a container and
installs it in a local cache (`$PLI_SYSROOT_DIR`, else `~/.pli/sysroots`):

| Target | Container | Provenance |
| --- | --- | --- |
| `x86_64-unknown-linux-gnu` | `debian:trixie-slim` | `musl-tools` + `musl-dev` 1.2.4 (`/usr/lib/x86_64-linux-musl/`) + LLVM compiler-rt builtins (renamed) |
| `aarch64-unknown-linux-gnu` | `ubuntu:24.04` (`--platform linux/arm64`) | `musl-dev` (`/usr/lib/aarch64-linux-musl/`) + LLVM compiler-rt builtins (renamed) |
| `x86_64-pc-windows-gnu` | `debian:trixie-slim` | `mingw-w64` (`/usr/x86_64-w64-mingw32/`) + `gcc-mingw-w64-x86-64` (`libgcc.a`); `fltused.o`/`chkstk.o` are cross-built from the sources in this dir |

The cache is not committed: it is a build input, like a package cache, and
the prepared tree is large (the Linux targets need the musl headers, the
Windows one the mingw-w64 headers). The macOS SDK is neither cached nor
vendored — it is not redistributable, so a Darwin target uses the SDK of the
build host (`xcrun --show-sdk-path`) and only a macOS build host can target
macOS.
