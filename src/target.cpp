// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// target.cpp — multi-target target rows (cross rows added later).
#include "target.h"

#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include <unordered_map>

// The cross-target list is generated from scripts/cross_targets.txt by CMake,
// so the build rules and the driver diagnostics cannot drift apart.
#include "pli_cross_targets.inc"

namespace pli {
// Initialize all LLVM targets for cross-compilation support.
void initializeAllTargets() {
  static const bool inited = [] {
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();
    llvm::InitializeAllTargets();
    llvm::InitializeAllDisassemblers();
    // Also ensure native target is fully initialized (asm info, etc.)
    // needed for in-process codegen on the host.
    llvm::InitializeNativeTarget();
    return true;
  }();
  (void)inited;
}

// The version in the OS component of a triple ("macosx14.0" -> "14.0",
// "darwin25.6.0" -> "25.6.0"), or "" when the triple carries none. LLVM's
// getMacOSXVersion() cannot tell "no version" from "the 10.4 default", and
// the default matters: an old deployment target makes lld emit the classic
// lazy binding, which needs the dyld_stub_binder symbol the vendored
// libSystem stub does not export.
static std::string darwinVersionOf(const llvm::Triple& t) {
  const std::string s = t.getTriple();
  size_t pos = 0;
  int field = 0;
  while (pos <= s.size()) {
    const size_t next = s.find('-', pos);
    const std::string comp =
        s.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
    if (field >= 2) { // the OS component
      const size_t d = comp.find_first_of("0123456789");
      if (d != std::string::npos)
        return comp.substr(d);
    }
    if (next == std::string::npos)
      break;
    pos = next + 1;
    ++field;
  }
  return "";
}

// Per-target descriptor cache keyed by triple string.
const TargetDesc& getTargetDesc(const llvm::Triple& t) {
  static std::unordered_map<std::string, TargetDesc> cache;
  const std::string key = t.getTriple();
  auto it = cache.find(key);
  if (it != cache.end())
    return it->second;

  TargetDesc d;
  d.defaultTriple = key;
  d.objFmt = t.getObjectFormat();
  switch (d.objFmt) {
  case llvm::Triple::ELF:
    d.lldDriver = "elf";
    d.systemLibs = {"-lm", "-lc"};
    d.linkerFlags = {"--gc-sections"};
    break;
  case llvm::Triple::COFF:
    d.lldDriver = "coff";
    d.systemLibs = {"kernel32.lib", "ucrt.lib", "vcruntime.lib", "msvcrt.lib",
                    "oldnames.lib", "legacy_stdio_definitions.lib"};
    // NOTE: /MD is a compiler flag, not a linker flag — lld-link rejects it.
    d.linkerFlags = {"/entry:mainCRTStartup", "/subsystem:console"};
    break;
  case llvm::Triple::MachO: {
    d.lldDriver = "macho";
    d.systemLibs = {"-lSystem", "-lm"};
    // The deployment target: the version the triple carries, else 15.0 (the
    // target IRGen stamps for a Darwin target without a version). It must
    // match the object files' minos, and the vendored libSystem stub needs
    // a modern one (see darwinVersionOf).
    std::string macosMinVer = "15.0";
    const std::string dv = darwinVersionOf(t);
    if (!dv.empty())
      macosMinVer = dv;
    d.linkerFlags = {"-dead_strip", "-platform_version", "macos", macosMinVer, macosMinVer};
    break;
  }
  default:
    break;
  }
  cache[key] = d;
  return cache[key];
}

std::unique_ptr<llvm::TargetMachine>
createTargetMachine(const std::string& triple, const std::string& optLevel, std::string& err,
                    bool marchNative) {
  initializeAllTargets();

  std::string lookupErr;
  llvm::Triple ttm(triple);
  const llvm::Target* target = llvm::TargetRegistry::lookupTarget(ttm, lookupErr);
  if (!target) {
    err = "cannot find target for triple '" + triple + "': " + lookupErr;
    return nullptr;
  }
  llvm::CodeGenOptLevel lvl = llvm::CodeGenOptLevel::Default;
  if (optLevel == "-O0")
    lvl = llvm::CodeGenOptLevel::None;
  else if (optLevel == "-O1")
    lvl = llvm::CodeGenOptLevel::Less;
  else if (optLevel == "-O3" || optLevel == "-Os")
    lvl = llvm::CodeGenOptLevel::Aggressive;
  llvm::TargetOptions opts;
  opts.FunctionSections = true;
  // --march=native: use host CPU's feature set (opt-in; default stays generic).
   std::string cpu = marchNative ? llvm::sys::getHostCPUName().str() : "generic";
  const char* features = "";
  return std::unique_ptr<llvm::TargetMachine>(target->createTargetMachine(
      ttm, cpu, features, opts, llvm::Reloc::PIC_, llvm::CodeModel::Small, lvl));
}

// ADR-234: the canonical arch name. LLVM's getArchName() returns the
// spelling the user typed, so "arm64-apple-darwin" and "aarch64-apple-darwin"
// — the same ABI — would land in different families. The arch *type* name is
// canonical ("aarch64"), and it matches the names the vendored sysroots and
// the builtins archives use.
std::string targetArchName(const llvm::Triple& t) {
  return llvm::Triple::getArchTypeName(t.getArch()).str();
}

// ADR-234: the ABI family of a triple. Everything that must agree for a
// binary to be valid — the runtime bitcode, the link sysroot, the CRT — is
// keyed by this family, not by the raw triple string: LLVM spells the same
// target several ways ("arm64-apple-darwin25.6.0" vs "arm64-apple-macosx26.0.0",
// "x86_64-pc-windows-gnu" vs "x86_64-w64-windows-gnu").
std::string targetKey(const llvm::Triple& t) {
  std::string os;
  switch (t.getOS()) {
  case llvm::Triple::Linux:
    os = "linux";
    break;
  case llvm::Triple::Darwin:
  case llvm::Triple::MacOSX:
    os = "darwin";
    break;
  case llvm::Triple::Win32:
    os = "windows";
    break;
  case llvm::Triple::FreeBSD:
    os = "freebsd";
    break;
  case llvm::Triple::NetBSD:
    os = "netbsd";
    break;
  case llvm::Triple::OpenBSD:
    os = "openbsd";
    break;
  case llvm::Triple::DragonFly:
    os = "dragonfly";
    break;
  case llvm::Triple::Fuchsia:
    os = "fuchsia";
    break;
  case llvm::Triple::AIX:
    os = "aix";
    break;
  case llvm::Triple::Haiku:
    os = "haiku";
    break;
  case llvm::Triple::Solaris:
    os = "solaris";
    break;
  default:
    os = "unknown";
    break;
  }
  // Android is an environment on top of Linux, and its libc (bionic) is not
  // the glibc/musl one: give it its own family.
  if (t.getEnvironment() == llvm::Triple::Android)
    os = "android";
  std::string key = os + "-" + targetArchName(t);
  // The Windows GNU (MinGW) and MSVC ABIs differ: the CRT, the import
  // libraries and some calling conventions are not interchangeable, so they
  // are separate families and never share a runtime blob.
  if (os == "windows" && t.getEnvironment() == llvm::Triple::MSVC)
    key += "-msvc";
  return key;
}

const std::vector<std::string>& crossTargetTriples() {
  static const std::vector<std::string> targets = [] {
    std::vector<std::string> v;
    for (const char* t : kCrossTargetTriples) {
      if (!t) // the generated list ends with a nullptr sentinel
        break;
      v.emplace_back(t);
    }
    return v;
  }();
  return targets;
}

bool isHostTarget(const llvm::Triple& t) {
  return targetKey(t) == targetKey(llvm::Triple(llvm::sys::getDefaultTargetTriple()));
}
} // namespace pli
