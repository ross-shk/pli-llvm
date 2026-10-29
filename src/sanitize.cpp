// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// sanitize.cpp — AddressSanitizer pass wiring and runtime discovery (B13).
#include "sanitize.h"

#include "llvm/Config/llvm-config.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Instrumentation/AddressSanitizer.h"
#include <cstdlib>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace pli {
void instrumentAddressSanitizer(llvm::Module& M) {
  // The ASan pass needs the same analysis registries the default pipeline
  // uses; no TargetMachine is required (ASan is target-independent).
  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;
  llvm::PassBuilder PB;
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  // LLVM 23 instruments only functions marked `sanitize_address`; mark every
  // definition that has not opted out (the driver opts the runtime blob out).
  for (llvm::Function& F : M)
    if (!F.isDeclaration() && !F.hasFnAttribute(llvm::Attribute::DisableSanitizerInstrumentation))
      F.addFnAttr(llvm::Attribute::SanitizeAddress);
  // No recovery: a finding stops the program (a test sees a non-zero exit).
  llvm::AddressSanitizerOptions opts;
  llvm::ModulePassManager mpm;
  mpm.addPass(llvm::AddressSanitizerPass(opts));
  mpm.run(M, MAM);
}

std::string addressSanitizerRuntime(const llvm::Triple& triple, const std::string& exeDir) {
  // Windows has no MinGW-compatible ASan runtime: the MSVC import library
  // does not match the MinGW driver, so the option stays diagnosed there.
  if (triple.getOS() == llvm::Triple::Win32)
    return "";
  const bool darwin = triple.isMacOSX() || triple.getOS() == llvm::Triple::Darwin;
  // Darwin ships the dynamic runtime; ELF ships a per-architecture archive.
  const std::string file = darwin ? "libclang_rt.asan_osx_dynamic.dylib"
                                  : "libclang_rt.asan-" + triple.getArchName().str() + ".a";
  std::vector<fs::path> dirs;
  if (!exeDir.empty())
    dirs.emplace_back(exeDir);
  // Same discovery as the profile runtime: $PLI_TOOLCHAIN_PREFIX, else the
  // baked build-time prefix, then <prefix>/lib/clang/<major>/lib/<os>/.
  std::string prefix;
  if (const char* env = std::getenv("PLI_TOOLCHAIN_PREFIX"))
    prefix = env;
#ifdef PLI_TOOLCHAIN_PREFIX
  if (prefix.empty())
    prefix = PLI_TOOLCHAIN_PREFIX;
#endif
  if (!prefix.empty()) {
    fs::path res = fs::path(prefix) / "lib" / "clang" / std::to_string(LLVM_VERSION_MAJOR) / "lib";
    dirs.push_back(res / (darwin ? "darwin" : "linux"));
  }
  for (const fs::path& d : dirs) {
    std::error_code ec;
    fs::path candidate = d / file;
    if (fs::exists(candidate, ec))
      return candidate.string();
  }
  return "";
}
} // namespace pli
