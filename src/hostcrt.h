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

// ADR-227: vendored sysroots (Zig-style self-containment). The link sysroot
// is EMBEDDED in pli(.exe) — release archives hold only the binary — so user
// links need no host toolchain (no build-essential CRT on Linux, no MSVC LIB
// on Windows, no Xcode CLT on macOS). An exe-relative musl//mingw//sdk/ dir
// next to the binary is a dev override (iterate on sysroot files without
// rebuilding); it wins when complete. All-or-nothing per OS: a partial dir
// falls back to the next source, then to the host recipe above.
struct VendoredSysroot {
  bool haveMusl = false;   // musl/{crt1.o,crti.o,crtn.o,libc.a,...}
  bool haveMingw = false;  // mingw/{crt2.o,...,libkernel32.a,...}
  std::string muslDir;     // set when haveMusl
  std::string mingwDir;    // set when haveMingw
};

// Probe exeDir (the pli executable's directory) for override sysroots.
// Never fails: an empty dir or absent files mean absent sysroots, and the
// caller falls back to the embedded set, then the host recipe.
VendoredSysroot resolveVendoredSysroot(const std::string& exeDir);

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
