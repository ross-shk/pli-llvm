// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM
// target.cpp — host-only target rows (cross rows added later).
#include "target.h"

#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"

namespace plic {
// Host descriptor, built once from the default triple.
// Use macosx15.0 for bitcode compatibility; SME features disabled explicitly.
static const TargetDesc kHostDesc = [] {
  TargetDesc d;
  d.defaultTriple = "arm64-apple-macosx15.0";
  llvm::Triple t(d.defaultTriple);
  d.objFmt = t.getObjectFormat();
  switch (d.objFmt) {
  case llvm::Triple::ELF:
    d.lldDriver = "elf";
    d.systemLibs = {"-lpthread", "-ldl", "-lm", "-lc"};
    d.linkerFlags = {"--gc-sections"};
    break;
  case llvm::Triple::MachO:
    d.lldDriver = "macho";
    d.systemLibs = {"-lSystem", "-lm"};
    d.linkerFlags = {"-dead_strip", "-platform_version", "macos", "13.0", "13.0"};
    break;
  case llvm::Triple::COFF:
    d.lldDriver = "coff";
    d.systemLibs = {"kernel32.lib", "ucrt.lib", "vcruntime.lib"};
    d.linkerFlags = {"/entry:mainCRTStartup", "/subsystem:console", "/MD"};
    break;
  default:
    break;
  }
  return d;
}();

const TargetDesc& getTargetDesc(const llvm::Triple&) { return kHostDesc; }

std::unique_ptr<llvm::TargetMachine>
createTargetMachine(const std::string& triple, const std::string& optLevel, std::string& err) {
  // One-time native-target init (host-only milestone).
  static const bool inited = [] {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    return true;
  }();
  (void)inited;

  std::string lookupErr;
  // Use the provided triple (darwin24 for macOS 15, which doesn't enable SME by default).
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
  // darwin24 (macOS 15) doesn't enable SME features by default, unlike macosx15.
  // No feature string needed.
  const char* features = "";
  // PIE objects match what the clang-subprocess backend produced.
  return std::unique_ptr<llvm::TargetMachine>(target->createTargetMachine(
      ttm, "generic", features, opts, llvm::Reloc::PIC_, llvm::CodeModel::Small, lvl));
}
} // namespace plic
