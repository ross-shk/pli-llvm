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
} // namespace pli
