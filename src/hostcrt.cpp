// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// hostcrt.cpp — host C-runtime/linker recipes for the in-process lld link.
//
// The old driver asked `clang -print-file-name=Scrt1.o` / `-print-search-dirs`
// to locate the CRT; that subprocess is gone (ADR-225). These tables probe the
// stable per-distro locations instead, first-hit-wins, with $PLI_SYSROOT
// prepended so a non-root host link can be redirected.
#include "hostcrt.h"

// Host.h moved from Support to TargetParser in LLVM 19; support both.
#if __has_include("llvm/TargetParser/Host.h")
#include "llvm/TargetParser/Host.h"
#else
#include "llvm/Support/Host.h"
#endif
#include "llvm/TargetParser/Triple.h"
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace pli {
namespace {
// Candidate search-root lists per family. Each is a set of <lib-dir> roots;
// the CRT startup objects live directly in them (Debian-multiarch, Fedora
// /usr/lib64, Arch /usr/lib).
struct CrtSearch {
  std::vector<std::string> roots; // candidate library roots, first-hit-wins
  // Expected CRT startup objects, in lld argv order. A name is mandatory
  // unless marked optional.
  std::vector<std::string> startObjects; // Scrt1.o, crti.o, crtbeginS.o
  std::vector<std::string> endObjects;   // crtendS.o, crtn.o
  std::string interp;                    // ELF interpreter
};

// First existing candidate for `name` under each root (sysroot-prepended).
std::string findInRoots(const std::vector<std::string>& roots,
                        const std::vector<std::string>& names, const std::string& sysroot) {
  for (const std::string& root : roots) {
    fs::path base = sysroot.empty() ? fs::path(root) : fs::path(sysroot) / root;
    for (const std::string& n : names) {
      fs::path p = base / n;
      std::error_code ec;
      if (fs::exists(p, ec))
        return p.string();
    }
  }
  return "";
}

// The first root that actually contains the CRT is the library-search root
// (for -L); prefer the sysroot-aware path.
std::vector<std::string> libDirsFromRoots(const std::vector<std::string>& roots,
                                          const std::string& sysroot) {
  std::vector<std::string> dirs;
  for (const std::string& root : roots) {
    fs::path base = sysroot.empty() ? fs::path(root) : fs::path(sysroot) / root;
    std::error_code ec;
    if (fs::is_directory(base, ec))
      dirs.push_back(base.string());
  }
  return dirs;
}

bool isHost(const llvm::Triple& t) { return t.getTriple() == llvm::sys::getDefaultTargetTriple(); }
} // namespace

HostCrt resolveHostCrt(const llvm::Triple& triple, const std::string& sysroot) {
  HostCrt out;

  // Host-only link: the CRT recipes describe the machine pli runs on. A
  // --triple for another OS/arch can't be linked here (no cross CRT), matching
  // the old behaviour of a missing -print path. Use -c for cross objects.
  if (!isHost(triple)) {
    out.err = "host C-runtime link is host-only: --triple '" + triple.getTriple() +
              "' cannot be linked here (cross compile with -c instead)";
    return out;
  }

  CrtSearch s;
  switch (triple.getObjectFormat()) {
  case llvm::Triple::ELF: {
    // Distro roots in preference order (Debian-multiarch first, then Fedora,
    // then Arch). gcc's lib dir is added so -lgcc / -lgcc_s resolve.
    s.roots = {"/usr/lib/" + std::string(triple.getArchName()) + "-linux-gnu", "/usr/lib64",
               "/usr/lib/x86_64-linux-gnu", "/usr/lib/aarch64-linux-gnu", "/usr/lib"};
    switch (triple.getArch()) {
    case llvm::Triple::x86_64:
      s.interp = "/lib64/ld-linux-x86-64.so.2";
      break;
    case llvm::Triple::aarch64:
      s.interp = "/lib/ld-linux-aarch64.so.1";
      break;
    case llvm::Triple::arm:
    case llvm::Triple::armeb:
    case llvm::Triple::thumb:
      s.interp = "/lib/ld-linux-armhf.so.3";
      break;
    case llvm::Triple::riscv64:
      s.interp = "/lib/ld-linux-riscv64-lp64d.so.1";
      break;
    default:
      break;
    }
    // PIE startup (matches clang): Scrt1.o + crtbeginS.o/crtendS.o.
    s.startObjects = {"Scrt1.o", "crti.o", "crtbeginS.o"};
    s.endObjects = {"crtendS.o", "crtn.o"};
    break;
  }
  case llvm::Triple::MachO:
    // Mach-O needs no CRT startup objects: lld synthesises the entry from the
    // SDK's libSystem (-lSystem, already in target.cpp). Nothing to probe.
    out.ok = true;
    return out;
  case llvm::Triple::COFF:
    // COFF startup comes from the MSVC runtime libs in target.cpp; lld-link
    // finds them via the LIB env var / -L dirs. Nothing to probe here.
    out.ok = true;
    return out;
  default:
    out.err = "no host C-runtime recipe for object format " +
              std::to_string((int)triple.getObjectFormat());
    return out;
  }

  // Resolve the mandatory startup objects; any missing -> clear diagnostic.
  std::string found;
  for (const std::string& n : s.startObjects) {
    std::string p = findInRoots(s.roots, {n}, sysroot);
    if (p.empty()) {
      out.err = "host C-runtime startup object '" + n +
                "' not found under /usr/lib* (install the C toolchain: "
                "build-essential / gcc on Linux)";
      return out;
    }
    out.startupObjects.push_back(p);
  }
  for (const std::string& n : s.endObjects) {
    std::string p = findInRoots(s.roots, {n}, sysroot);
    if (!p.empty())
      out.endObjects.push_back(p);
  }
  // Best-effort: every root that exists becomes a -L dir; gcc's libdir carries
  // libgcc_s even if the glibc root differs.
  out.libDirs = libDirsFromRoots(s.roots, sysroot);
  // The interpreter lives outside the sysroot (it's the host loader); only
  // use it when it actually exists, else lld omits the INTERP (host PIE).
  if (!s.interp.empty()) {
    std::error_code ec;
    if (fs::exists(s.interp, ec))
      out.interpreter = s.interp;
  }
  out.ok = true;
  return out;
}
} // namespace pli
