// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM
// target.cpp — multi-target target rows (cross rows added later).
#include "target.h"

#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include <unordered_map>

namespace plic {
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
    d.systemLibs = {"kernel32.lib", "ucrt.lib", "vcruntime.lib"};
    d.linkerFlags = {"/entry:mainCRTStartup", "/subsystem:console", "/MD"};
    break;
  case llvm::Triple::MachO: {
    d.lldDriver = "macho";
    d.systemLibs = {"-lSystem", "-lm"};
    // Derive the macOS version from the triple (translates Darwin N -> macOS)
    // so the -platform_version matches the object files' minos instead of
    // hardcoded value that mismatches the host OS.
    std::string macosMinVer = "13.0";
    llvm::VersionTuple vt;
    if (t.getMacOSXVersion(vt) && !vt.empty())
      macosMinVer = vt.getAsString();
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
createTargetMachine(const std::string& triple, const std::string& optLevel, std::string& err) {
  // One-time all-target init for cross-compilation.
  initializeAllTargets();

  std::string lookupErr;
  llvm::Triple ttm(triple);
  const llvm::Target* target = llvm::TargetRegistry::lookupTarget(ttm, lookupErr);
  if (!target) {
    err = "cannot find target for triple '" + triple + "': " + lookupErr;
    return nullptr;
  }
  // Map the driver -O flag onto the LLVM codegen level.
  llvm::CodeGenOptLevel lvl = llvm::CodeGenOptLevel::Default;
  if (optLevel == "-O0")
    lvl = llvm::CodeGenOptLevel::None;
  else if (optLevel == "-O1")
    lvl = llvm::CodeGenOptLevel::Less;
  else if (optLevel == "-O3" || optLevel == "-Os")
    lvl = llvm::CodeGenOptLevel::Aggressive;
  llvm::TargetOptions opts;
  opts.FunctionSections = true; // one section per function for --gc-sections / dead_strip
  const char* features = "";
  // PIE objects match what the clang-subprocess backend produced.
  return std::unique_ptr<llvm::TargetMachine>(target->createTargetMachine(
      ttm, "generic", features, opts, llvm::Reloc::PIC_, llvm::CodeModel::Small, lvl));
}
} // namespace plic
