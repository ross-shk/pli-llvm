// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// pgo.cpp — in-process PGO: options, profile validation, runtime location.
#include "pgo.h"

#include "codegen.h"
#include "embedded_runtime.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/Module.h"
#include "llvm/ProfileData/InstrProfReader.h"
#include "llvm/Support/PGOOptions.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/TargetParser/Triple.h"
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace pli {
std::optional<llvm::PGOOptions> makePgoOptions(PgoMode mode, const std::string& profilePath) {
  switch (mode) {
  case PgoMode::Generate:
    // IR-level instrumentation; the profile runtime writes default.profraw
    // when the instrumented binary exits (LLVM_PROFILE_FILE overrides).
    return llvm::PGOOptions("", "", "", "", llvm::PGOOptions::IRInstr);
  case PgoMode::Use:
    return llvm::PGOOptions(profilePath, "", "", "", llvm::PGOOptions::IRUse);
  case PgoMode::None:
    return std::nullopt;
  }
  return std::nullopt;
}

bool validateUseProfile(const std::string& profilePath, const llvm::Module& M,
                        const std::set<std::string>& runtimeNames, std::string& err) {
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> vfs = llvm::vfs::getRealFileSystem();
  auto readerOrErr = llvm::InstrProfReader::create(profilePath, *vfs);
  if (!readerOrErr) {
    err = "--fprofile-use=" + profilePath +
          ": cannot read profile (raw .profraw or indexed "
          ".profdata expected)";
    return false;
  }
  std::unique_ptr<llvm::InstrProfReader> reader = std::move(*readerOrErr);
  if (!reader->isIRLevelProfile()) {
    err = "--fprofile-use=" + profilePath + ": not an IR-level profile";
    return false;
  }
  // A profiled USER function must exist in this module; otherwise the profile
  // belongs to different sources (stale) and use-mode would silently optimize
  // nothing. Profile record names carry a `pli;` prefix the module functions
  // lack (`pli;PLI_FOO` vs `PLI_FOO`), so strip it for lookup. Records that
  // resolve to embedded-runtime functions do not count: the bitcode runtime
  // is merged into every MAIN module (and the `main` shim is emitted for
  // every program), so any pli profile trivially overlaps any other through
  // those — only a user-function match proves the profile belongs here.
  bool anyUserRec = false, anyUserMatch = false;
  for (const llvm::NamedInstrProfRecord& rec : *reader) {
    std::string name = rec.Name.str();
    if (name.rfind("pli;", 0) == 0)
      name = name.substr(4);
    if (runtimeNames.count(name))
      continue;
    if (name == "main")
      continue;
    anyUserRec = true;
    if (M.getFunction(name)) {
      anyUserMatch = true;
      break;
    }
  }
  if (anyUserRec && !anyUserMatch) {
    err = "--fprofile-use=" + profilePath +
          ": profile matches no function in this module "
          "(stale profile?)";
    return false;
  }
  return true;
}

std::set<std::string> runtimeFuncNames(const llvm::Triple& triple) {
  std::set<std::string> names;
  llvm::LLVMContext ctx;
  std::string err;
  if (auto rt = parseEmbeddedRuntime(triple, ctx, err)) {
    for (const llvm::Function& f : *rt)
      if (!f.isDeclaration())
        names.insert(f.getName().str());
  }
  // The platform TU is a second embedded blob; its pli_plat_* functions are
  // merged into every MAIN module too, so they must not prove a stale profile
  // belongs here.
  if (const RuntimeBlob* blob = selectPlatformForTarget(triple)) {
    llvm::MemoryBufferRef buf(llvm::StringRef(reinterpret_cast<const char*>(blob->data), blob->len),
                              "pli_rtplat.bc");
    if (auto modOrErr = llvm::parseBitcodeFile(buf, ctx)) {
      for (const llvm::Function& f : **modOrErr)
        if (!f.isDeclaration())
          names.insert(f.getName().str());
    } else {
      llvm::consumeError(modOrErr.takeError());
    }
  }
  return names;
}

std::string profileRuntimeLib(const llvm::Triple& triple, const std::string& exeDir) {
  std::string arch = triple.getArchName().str();
  // Release layout first: the archive ships next to pli(.exe) under a uniform
  // name (pack step renames it out of the toolchain resource dir).
  std::vector<std::string> candidates;
  if (!exeDir.empty())
    candidates.push_back((fs::path(exeDir) / ("clang_rt.profile-" + arch + ".a")).string());
  // Toolchain resource layout: <prefix>/lib/clang/<major>/lib/<os>/, whose
  // filename varies by OS (osx / <arch> / windows conventions). $PLI_SYSROOT
  // aside, the prefix is $PLI_TOOLCHAIN_PREFIX, else the baked build-time
  // prefix (dev machines), so developers need no env setup.
  std::string prefix;
  if (const char* env = std::getenv("PLI_TOOLCHAIN_PREFIX"))
    prefix = env;
#ifdef PLI_TOOLCHAIN_PREFIX
  if (prefix.empty())
    prefix = PLI_TOOLCHAIN_PREFIX;
#endif
  if (!prefix.empty()) {
    fs::path res = fs::path(prefix) / "lib" / "clang" / std::to_string(LLVM_VERSION_MAJOR) / "lib";
    if (triple.getOS() == llvm::Triple::Darwin || triple.getOS() == llvm::Triple::MacOSX)
      candidates.push_back((res / "darwin" / "libclang_rt.profile_osx.a").string());
    else if (triple.getOS() == llvm::Triple::Win32)
      candidates.push_back((res / "windows" / ("clang_rt.profile-" + arch + ".lib")).string());
    else
      candidates.push_back((res / "linux" / ("libclang_rt.profile-" + arch + ".a")).string());
  }
  for (const std::string& c : candidates) {
    std::error_code ec;
    if (fs::exists(c, ec))
      return c;
  }
  return "";
}
} // namespace pli
