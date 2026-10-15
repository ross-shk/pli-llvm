// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// codegen.cpp — in-process object emission + runtime-once helpers + lld link.
#include "codegen.h"

#include "embedded_runtime.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Triple.h"

#if PLI_HAVE_LLD
#include "lld/Common/Driver.h"
// brew's lld ships only Common headers, so declare each driver's `link` entry
// point with the public macro (no lld/ELF/Driver.h in the install).
LLD_HAS_DRIVER(elf)
LLD_HAS_DRIVER(macho)
LLD_HAS_DRIVER(coff)
#endif

namespace pli {
// Map the driver `-O` flag onto the LLVM pass-builder level.
static llvm::OptimizationLevel toOptLevel(const std::string& s) {
  if (s == "-O0")
    return llvm::OptimizationLevel::O0;
  if (s == "-O1")
    return llvm::OptimizationLevel::O1;
  if (s == "-O3")
    return llvm::OptimizationLevel::O3;
  return llvm::OptimizationLevel::O2;
}

void optimizeModule(llvm::Module& M, llvm::TargetMachine& TM,
                    const std::string& optLevel) {
  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;
  // ADR-213: forward the TargetMachine so buildPerModuleDefaultPipeline
  // registers target-specific TargetIRAnalysis (SIMD features). Without TM,
  // the default TTI reports zero vector registers and the loop vectorizer
  // silently skips every loop.
  llvm::PassBuilder PB(&TM);
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  llvm::ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(toOptLevel(optLevel));
  MPM.run(M, MAM);
}

bool emitObject(llvm::Module& M, llvm::TargetMachine& TM, llvm::raw_pwrite_stream& out,
                std::string& err) {
  llvm::legacy::PassManager pm;
  // Integrated assembler: object emitted in-process, no `as`/`clang` subprocess.
  if (TM.addPassesToEmitFile(pm, out, nullptr, llvm::CodeGenFileType::ObjectFile)) {
    err = "TargetMachine cannot emit an object file for this target";
    return false;
  }
  pm.run(M);
  return true;
}

// Split runtime: arch + OS-family match for a platform TU (mirrors the
// irgen.cpp portable check; LLVM 23 splits Darwin/MacOSX, same ABI family).
static bool platformTripleMatches(const llvm::Module& mod, const llvm::Triple& target) {
  llvm::Triple modTriple(mod.getTargetTriple());
  auto osKey = [](llvm::Triple::OSType os) -> int {
    return (os == llvm::Triple::Darwin || os == llvm::Triple::MacOSX) ? (int)llvm::Triple::Darwin
                                                                      : (int)os;
  };
  return modTriple.getArch() == target.getArch() && osKey(modTriple.getOS()) == osKey(target.getOS());
}

// Strip the build host's target-cpu / target-features / probe-stack /
// tune-cpu (same four as the portable paths). No NoInline stamp: platform
// wrappers stay inlinable into their portable callers, as when they were
// header-inline.
static void stripPlatformAttrs(llvm::Module& mod) {
  for (auto& F : mod) {
    if (F.hasFnAttribute("target-cpu"))
      F.removeFnAttr("target-cpu");
    if (F.hasFnAttribute("target-features"))
      F.removeFnAttr("target-features");
    if (F.hasFnAttribute("probe-stack"))
      F.removeFnAttr("probe-stack");
    if (F.hasFnAttribute("tune-cpu"))
      F.removeFnAttr("tune-cpu");
  }
}

std::unique_ptr<llvm::Module> loadPlatformModule(llvm::LLVMContext& ctx,
                                                 const llvm::Triple& targetTriple,
                                                 const std::string& platformBc, std::string& err) {
  std::unique_ptr<llvm::Module> plat;
  std::string platName;
  if (!platformBc.empty()) {
    llvm::SMDiagnostic smErr;
    plat = llvm::parseIRFile(platformBc, smErr, ctx);
    if (!plat) {
      err = "cannot load the platform runtime '" + platformBc + "': " + smErr.getMessage().str();
      return nullptr;
    }
    platName = platformBc;
    // A platform TU carries no LLVM stamp (only the portable blob has
    // pli_llvm_version); the triple match is the compatibility check, so a
    // file built by a nearby clang for the same target links cleanly.
    if (!platformTripleMatches(*plat, targetTriple)) {
      err = "platform runtime target '" + plat->getTargetTriple().str() + "' does not match '" +
            targetTriple.getTriple() + "' — rebuild it for the target";
      return nullptr;
    }
  } else {
    const RuntimeBlob* blob = selectPlatformForTarget(targetTriple);
    if (!blob)
      return nullptr; // no embedded platform: caller reports per-path
    llvm::MemoryBufferRef buf(
        llvm::StringRef(reinterpret_cast<const char*>(blob->data), blob->len), "pli_rtplat.bc");
    llvm::Expected<std::unique_ptr<llvm::Module>> parsed = llvm::parseBitcodeFile(buf, ctx);
    if (!parsed) {
      err = "cannot parse embedded pli_rtplat.bc";
      llvm::consumeError(parsed.takeError());
      return nullptr;
    }
    plat = std::move(*parsed);
    platName = "embedded pli_rtplat.bc";
  }
  stripPlatformAttrs(*plat);
  return plat;
}

bool isPlatformSeamSymbol(const std::string& name) {
  // pli_plat_* plus the threading backend (same names as the old header).
  static const char* const kThreadSeam[] = {
      "pli_mutex_init",     "pli_mutex_lock",      "pli_mutex_unlock",  "pli_cond_init",
      "pli_cond_wait",      "pli_cond_signal",     "pli_cond_broadcast", "pli_call_once",
      "pli_spawn",          "pli_spawn_detached",  "pli_thread_detach", "pli_sleep_ms",
  };
  if (name.rfind("pli_plat_", 0) == 0)
    return true;
  for (const char* t : kThreadSeam)
    if (name == t)
      return true;
  return false;
}

std::string checkPlatformSeamComplete(const llvm::Module& M) {
  std::string missing;
  for (const auto& F : M)
    if (F.isDeclaration() && isPlatformSeamSymbol(F.getName().str())) {
      if (!missing.empty())
        missing += ", ";
      missing += F.getName().str();
    }
  if (missing.empty())
    return "";
  return "incomplete platform runtime, still undefined: " + missing +
         " — implement all of runtime/pli_rt_plat.h (or drop --rt-platform)";
}

bool linkEmbeddedLibPLI(llvm::Module& M, const llvm::Triple& targetTriple, std::string& err,
                        const std::string& platformBc) {
  const RuntimeBlob* blob = selectRuntimeForTarget(targetTriple);
  if (!blob) {
    err = "no embedded runtime for target: " + targetTriple.getTriple();
    return false;
  }
  // Parse against M's own context so Linker can merge directly.
  llvm::MemoryBufferRef buf(llvm::StringRef(reinterpret_cast<const char*>(blob->data), blob->len),
                            "pli_runtime.bc");
  llvm::Expected<std::unique_ptr<llvm::Module>> parsed =
      llvm::parseBitcodeFile(buf, M.getContext());
  if (!parsed) {
    err = "cannot parse embedded pli_runtime.bc";
    llvm::consumeError(parsed.takeError());
    return false;
  }
  // Strip per-function target attributes from the runtime functions before
  // linking. The bitcode is compiled for apple-m1 with probe-stack=__chkstk_darwin;
  // LLVM 23's AArch64 backend mis-handles these when a function has a large
  // stack frame ("Unsupported stack probing method"). Removing target-cpu,
  // target-features, probe-stack, and tune-cpu forces codegen to use only the
  // TargetMachine's defaults. Without stripping tune-cpu, LLVM's inliner
  // refuses to honour AlwaysInline on the runtime wrappers (the host's
  // tune-cpu differs from the link-time target). Mark them noinline so they
  // stay as distinct, linkable runtime symbols (the dead-strip driver test
  // relies on this).
  for (auto& F : **parsed) {
    if (F.hasFnAttribute("target-cpu"))
      F.removeFnAttr("target-cpu");
    if (F.hasFnAttribute("target-features"))
      F.removeFnAttr("target-features");
    if (F.hasFnAttribute("probe-stack"))
      F.removeFnAttr("probe-stack");
    if (F.hasFnAttribute("tune-cpu"))
      F.removeFnAttr("tune-cpu");
    F.addFnAttr(llvm::Attribute::NoInline);
  }
  // Pull in only referenced runtime functions (replaces ADR-079 dead-strip).
  if (llvm::Linker::linkModules(M, std::move(*parsed), llvm::Linker::LinkOnlyNeeded)) {
    err = "cannot link embedded pli_runtime.bc";
    return false;
  }
  // Split runtime: the portable blob's pli_plat_*/thread references resolve
  // from the platform TU (default embedded blob or a custom --rt-platform
  // file), linked the same LinkOnlyNeeded way so unused pieces still drop.
  std::unique_ptr<llvm::Module> plat =
      loadPlatformModule(M.getContext(), targetTriple, platformBc, err);
  if (!plat) {
    if (err.empty())
      err = "no embedded platform runtime for target: " + targetTriple.getTriple();
    return false;
  }
  if (llvm::Linker::linkModules(M, std::move(plat), llvm::Linker::LinkOnlyNeeded)) {
    err = "cannot link the platform runtime into the module";
    return false;
  }
  if (std::string seamErr = checkPlatformSeamComplete(M); !seamErr.empty()) {
    err = seamErr;
    return false;
  }
  // Re-apply the audited P0 side-table facts (kRuntimeAttrs, irgen.cpp) on
  // the now-defined runtime functions. Linking replaces our declarations
  // (which carried applyRuntimeAttrs facts) with bitcode definitions carrying
  // clang's default attrs — without this, every linked pli_* call models
  // unknown memory effects and blocks LICM/GVN (e.g. an INDEX slow-path call
  // pins memchr+memcmp inside hot loops). Unknown names (user functions) are
  // left alone. Mirrors IRGen::linkRuntimeBitcode (P0).
  for (auto& f : M.functions())
    if (!f.isDeclaration())
      applyRuntimeFnAttrs(f);
  return true;
}

std::unique_ptr<llvm::Module> parseEmbeddedRuntime(const llvm::Triple& targetTriple,
                                                   llvm::LLVMContext& ctx, std::string& err) {
  const RuntimeBlob* blob = selectRuntimeForTarget(targetTriple);
  if (!blob) {
    err = "no embedded runtime for target: " + targetTriple.getTriple();
    return nullptr;
  }
  llvm::MemoryBufferRef buf(llvm::StringRef(reinterpret_cast<const char*>(blob->data), blob->len),
                            "pli_runtime.bc");
  auto modOrErr = llvm::parseBitcodeFile(buf, ctx);
  if (!modOrErr) {
    err = "cannot parse embedded pli_runtime.bc";
    llvm::consumeError(modOrErr.takeError());
    return nullptr;
  }
  return std::move(*modOrErr);
}

bool emitRuntimeObject(llvm::TargetMachine& TM, const llvm::Triple& targetTriple,
                       llvm::raw_pwrite_stream& out, std::string& err,
                       const std::string& platformBc) {
  llvm::LLVMContext ctx;
  auto rt = parseEmbeddedRuntime(targetTriple, ctx, err);
  if (!rt)
    return false;
  // Strip the same per-function attributes that break LLVM 23 codegen
  // (see linkEmbeddedLibPLI): target-cpu/target-features/probe-stack/tune-cpu
  // from the apple-m1-compiled runtime trigger "Unsupported stack probing
  // method" and defeat AlwaysInline honoring by the inliner.
  for (auto& F : *rt) {
    if (F.hasFnAttribute("target-cpu"))
      F.removeFnAttr("target-cpu");
    if (F.hasFnAttribute("target-features"))
      F.removeFnAttr("target-features");
    if (F.hasFnAttribute("probe-stack"))
      F.removeFnAttr("probe-stack");
    if (F.hasFnAttribute("tune-cpu"))
      F.removeFnAttr("tune-cpu");
    F.addFnAttr(llvm::Attribute::NoInline);
  }
  // Split runtime: merge the platform TU (default embedded blob or a custom
  // --rt-platform file) so the object defines the seam too; unresolved seam
  // symbols would otherwise surface only as native link errors.
  std::unique_ptr<llvm::Module> plat = loadPlatformModule(ctx, targetTriple, platformBc, err);
  if (!plat) {
    if (err.empty())
      err = "no embedded platform runtime for target: " + targetTriple.getTriple();
    return false;
  }
  if (llvm::Linker::linkModules(*rt, std::move(plat), llvm::Linker::LinkOnlyNeeded)) {
    err = "cannot link the platform runtime into the runtime object";
    return false;
  }
  rt->setDataLayout(TM.createDataLayout());
  return emitObject(*rt, TM, out, err);
}

bool linkExecutable(llvm::ArrayRef<const char*> args, int objFmt, std::string& err) {
#if PLI_HAVE_LLD
  // `args` are exactly the argv lld would receive (program name, then inputs).
  // `exitEarly=false` keeps the driver alive after a failed link; diagnostics
  // go to stderr (disableOutput=false).
  switch (static_cast<llvm::Triple::ObjectFormatType>(objFmt)) {
  case llvm::Triple::ELF:
    return lld::elf::link(args, llvm::outs(), llvm::errs(), false, false);
  case llvm::Triple::MachO:
    return lld::macho::link(args, llvm::outs(), llvm::errs(), false, false);
  case llvm::Triple::COFF:
    return lld::coff::link(args, llvm::outs(), llvm::errs(), false, false);
  default:
    err = "unsupported object format";
    return false;
  }
#else
  (void)args;
  (void)objFmt;
  err = "in-process link needs lld (brew install lld), falling back to clang link";
  return false;
#endif
}
} // namespace pli
