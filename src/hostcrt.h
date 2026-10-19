// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// hostcrt.h — host C-runtime/linker recipes for the in-process lld link.
//
// ADR-225: pli links executables in-process with lld and must locate the
// host C runtime (startup objects + system library dirs) WITHOUT shelling out
// to `clang -print-*`. This module owns the per-platform, per-arch recipe.
#pragma once
#include <string>
#include <vector>

namespace llvm {
class Triple;
}

namespace pli {
// The CRT files + library search dirs + dynamic interpreter lld needs for a
// host link. Resolved by probing stable filesystem locations, never a
// subprocess. `ok` is false (with a human diagnostic in `err`) when any
// mandatory CRT startup object is missing; library dirs are best-effort.
struct HostCrt {
  bool ok = false;
  std::string err;
  // Startup objects, in lld argv order (empty when absent is acceptable only
  // for the optional crtbegin/crtend pair, which some toolchains omit).
  std::vector<std::string> startupObjects; // Scrt1.o, crti.o, crtbeginS.o ...
  std::vector<std::string> endObjects;     // crtendS.o, crtn.o
  std::vector<std::string> libDirs;        // -L dirs (libc, libm, libgcc)
  std::string interpreter;                 // ELF -dynamic-linker, empty if none
};

// Resolve the host C-runtime recipe for `triple`. `sysroot` (from $PLI_SYSROOT,
// else empty) is prepended to every candidate path so a non-root host link
// can be redirected. Host-only: a non-host triple yields ok=false with a
// "host link only" diagnostic.
HostCrt resolveHostCrt(const llvm::Triple& triple, const std::string& sysroot);

// ADR-227/ADR-234: the link sysroot of a target — the directory holding the
// CRT startup objects and the system libraries. Three kinds:
//   "musl"  crt1.o crti.o crtn.o libc.a libpli_builtins-<arch>.a
//   "mingw" crt2.o crtbegin.o crtend.o fltused.o chkstk.o lib*.a
//   "sdk"   macOS .tbd stubs (usr/lib/libSystem.tbd), used as -syslibroot
struct SysrootSet {
  bool ok = false;
  std::string kind;   // "musl" | "mingw" | "sdk"
  std::string dir;    // directory holding the link inputs
  std::string origin; // "explicit" | "override" | "embedded" | "cache"
};

// Probe a directory for a link sysroot: the files present decide the kind.
// A directory that holds neither a musl nor a mingw CRT is not a sysroot.
// `arch` is the LLVM arch name of the target ("x86_64", "aarch64", ...): a
// musl set that carries builtins archives must carry the target's.
SysrootSet probeSysrootDir(const std::string& dir, const std::string& origin,
                            const std::string& arch = "");

// The prepared cross sysroot cache: $PLI_SYSROOT_DIR, else ~/.pli/sysroots,
// then the per-target subdirectory <cache>/<targetKey>/lib.
std::string sysrootCacheRoot();
// The cache directories to probe for a target, in order: the triple as given,
// then the canonical triples of the same ABI family (a target may be spelled
// differently, e.g. x86_64-unknown-linux-musl vs -gnu).
std::vector<std::string> sysrootCacheCandidates(const llvm::Triple& t);

// The embedded sysroot (embedded_sysroot.h) materialized to a temp dir.
// name is one of "musl", "mingw", "sdk" (which link path it feeds); dir is
// empty when this build embedded none or the write failed — the caller
// falls back to the host recipe. Remove with cleanupMaterializedSysroot
// once the link is done (best-effort, like other temp link inputs).
struct EmbeddedSysroot {
  std::string name;
  std::string dir;
};
EmbeddedSysroot materializeEmbeddedSysroot();
void cleanupMaterializedSysroot(const std::string& dir);
} // namespace pli
