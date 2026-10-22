// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// hostcrt.cpp — host C-runtime/linker recipes for the in-process lld link.
//
// The old driver asked `clang -print-file-name=Scrt1.o` / `-print-search-dirs`
// to locate the CRT; that subprocess is gone (ADR-225). These tables probe the
// stable per-distro locations instead, first-hit-wins, with $PLI_SYSROOT
// prepended so a non-root host link can be redirected.
#include "hostcrt.h"

#include "embedded_sysroot.h"
// Host.h moved from Support to TargetParser in LLVM 19; support both.
#if __has_include("llvm/TargetParser/Host.h")
#include "llvm/TargetParser/Host.h"
#else
#include "llvm/Support/Host.h"
#endif
#include "llvm/TargetParser/Triple.h"
#include "llvm/Support/FileSystem.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

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

// Debian/Ubuntu/Fedora keep the GCC startup shims (crtbegin(S).o, crtendS.o)
// and libgcc.a/libgcc_s.so only in GCC's private libdir, never the multiarch
// dir. The recipe must search it or a stock host PIE / -shared link cannot
// resolve crtbeginS.o. The GCC version is part of the path, so enumerate the
// versioned subdirs (a missing dir simply appends nothing).
void appendGccLibDirs(std::vector<std::string>& roots, const std::string& gnuTriple) {
  const fs::path root = fs::path("/usr/lib/gcc") / gnuTriple;
  std::error_code ec;
  if (!fs::is_directory(root, ec))
    return;
  std::vector<std::string> vers;
  for (const fs::directory_entry& e : fs::directory_iterator(root, ec))
    if (e.is_directory(ec))
      vers.push_back(e.path().filename().string());
  std::sort(vers.begin(), vers.end());
  for (const std::string& v : vers)
    roots.push_back("/usr/lib/gcc/" + gnuTriple + "/" + v);
}

bool isHost(const llvm::Triple& t) { return t.getTriple() == llvm::sys::getDefaultTargetTriple(); }

// ADR-227: every file below must exist for the sysroot to count (mirrors the
// release pack list; a partial dir falls back to the host recipe silently).
bool allExist(const fs::path& dir, const std::vector<std::string>& names) {
  for (const std::string& n : names) {
    std::error_code ec;
    if (!fs::is_regular_file(dir / n, ec))
      return false;
  }
  return true;
}
} // namespace

// The builtins archive is arch-specific (embedded per build arch): a
// foreign-arch sysroot must not count — its link would fail later on the
// missing arch file. Matches the CMake-embedded name (PLI_BUILD_ARCH).
bool haveArchBuiltins(const fs::path& musl) {
  const std::string want =
      "libpli_builtins-" +
      llvm::Triple(llvm::sys::getDefaultTargetTriple()).getArchName().str() + ".a";
  std::error_code ec;
  return fs::is_regular_file(musl / want, ec);
}

// musl static (Linux): startup objects + the single libc archive + empty
// libm/pthread/dl/rt stubs (separate files, so user -l flags resolve) +
// our renamed builtins archive for 128-bit helpers (__udivti3 in
// wide-decimal formatting, which musl omits; renamed so the hostcrt
// clang-shadow check never trips on the filename).
bool detectMusl(const fs::path& musl) {
  return allExist(musl, {"crt1.o", "crti.o", "crtn.o", "libc.a"}) && haveArchBuiltins(musl);
}

// mingw-w64 (Windows): CRT startup + import libs + GCC helpers. fltused.o
// is a one-word stub (int _fltused) for the MSVC-ABI marker clang emits;
// chkstk.o is a jmp thunk (__chkstk -> ___chkstk_ms) for the GNU-named
// stack probe of dynamic alloca frames; libgcc.a covers the MSVC-named
// probe plus __udivti3/___chkstk_ms, so no compiler-rt builtins ride.
bool detectMingw(const fs::path& mingw) {
  return allExist(mingw, {"crt2.o", "crtbegin.o", "crtend.o", "fltused.o", "chkstk.o", "libgcc.a",
                          "libkernel32.a", "libmsvcrt.a", "libmingw32.a", "libmingwex.a",
                          "libmoldname.a", "libucrt.a", "libadvapi32.a", "libshell32.a",
                          "libuser32.a"});
}

VendoredSysroot resolveVendoredSysroot(const std::string& exeDir) {
  VendoredSysroot out;
  if (exeDir.empty())
    return out;
  fs::path musl = fs::path(exeDir) / "musl";
  fs::path mingw = fs::path(exeDir) / "mingw";
  if (detectMusl(musl)) {
    out.haveMusl = true;
    out.muslDir = musl.string();
  }
  if (detectMingw(mingw)) {
    out.haveMingw = true;
    out.mingwDir = mingw.string();
  }
  return out;
}

EmbeddedSysroot materializeEmbeddedSysroot() {
  EmbeddedSysroot out;
  size_t n = 0;
  const SysrootFile* files = embeddedSysrootFiles(&n);
  const char* setName = embeddedSysrootName();
  if (!files || n == 0 || !setName || !*setName)
    return out;
  std::error_code ec;
  fs::path base = fs::temp_directory_path(ec);
  if (ec)
    return out;
  // One link per invocation: a unique directory under the temp dir. The name
  // is random and the directory is created atomically (create_directory fails
  // if the name exists), so neither a stale crash leftover nor a name planted
  // in the temp dir can be mistaken for ours. A predictable
  // "<tmp>/pli-sysroot-<pid>" path let a planted symlink redirect the writes
  // below. (Relative paths inside the set, e.g. sdk's usr/lib/..., are
  // recreated below.)
  fs::path dir;
  for (int attempt = 0; attempt < 8 && dir.empty(); ++attempt) {
    llvm::SmallString<128> candBuf;
    llvm::sys::fs::createUniquePath((base / "pli-sysroot-%%%%%%").string(), candBuf, false);
    fs::path cand(candBuf.str().str());
    std::error_code cec;
    // create_directory returns false when the name is already taken.
    if (!fs::create_directory(cand, cec))
      continue; // the name was taken: try another
    dir = cand;
  }
  if (dir.empty())
    return out;
  auto fail = [&]() {
    std::error_code cec;
    fs::remove_all(dir, cec);
    return EmbeddedSysroot();
  };
  for (size_t i = 0; i < n; ++i) {
    fs::path p = dir / files[i].path;
    fs::create_directories(p.parent_path(), ec);
    if (ec)
      return fail();
    std::ofstream os(p, std::ios::binary | std::ios::trunc);
    if (!os)
      return fail();
    os.write(reinterpret_cast<const char*>(files[i].data),
             static_cast<std::streamsize>(files[i].len));
    os.close();
    if (!os)
      return fail();
  }
  out.name = setName;
  out.dir = dir.string();
  return out;
}

void cleanupMaterializedSysroot(const std::string& dir) {
  if (dir.empty())
    return;
  // Only remove what materializeEmbeddedSysroot created: a directory directly
  // under the temp dir whose name starts with the marker prefix. A substring
  // test would also match a path like "/tmp/evil-pli-sysroot-x".
  fs::path p(dir);
  std::error_code ec;
  fs::path base = fs::temp_directory_path(ec);
  if (ec)
    return;
  // The path comparison is element-wise, but the temp dir path may carry a
  // trailing separator ("/var/.../T/"), which makes the native strings
  // differ. Compare without it.
  auto withoutTrailingSep = [](const fs::path& q) {
    std::string s = q.string();
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\'))
      s.pop_back();
    return s;
  };
  if (withoutTrailingSep(p.parent_path()) != withoutTrailingSep(base))
    return;
  if (p.filename().string().rfind("pli-sysroot-", 0) != 0)
    return;
  fs::remove_all(p, ec);
}

std::string findMacOSSdk() {
  // Probe stable SDK locations: CLT first (always present on dev machines),
  // then Xcode.app (full IDE / server installs use the Xcode SDK). Each root
  // must contain usr/lib (libSystem.tbd lives there) to count as a valid SDK.
  // No subprocess (ADR-225) — this replaces `xcrun --show-sdk-path`.
  const std::vector<std::string> roots = {
    "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk",
    "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk",
  };
  for (const std::string& root : roots) {
    std::error_code ec;
    if (fs::is_directory(fs::path(root) / "usr" / "lib", ec))
      return root;
  }
  return "";
}

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
    // then Arch). GCC's versioned libdir is appended so crtbegin(S).o and
    // -lgcc / -lgcc_s resolve on a stock host.
    const std::string gnuTriple = std::string(triple.getArchName()) + "-linux-gnu";
    s.roots = {"/usr/lib/" + gnuTriple, "/usr/lib64", "/usr/lib/x86_64-linux-gnu",
               "/usr/lib/aarch64-linux-gnu", "/usr/lib"};
    appendGccLibDirs(s.roots, gnuTriple);
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
