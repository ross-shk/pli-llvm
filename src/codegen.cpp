// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// codegen.cpp — in-process object emission + runtime-once helpers + lld link.
#include <vector>

#include "codegen.h"

#include "embedded_runtime.h"
#include "llvm/Analysis/ModuleSummaryAnalysis.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/PGOOptions.h"
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
LLD_HAS_DRIVER(mingw)
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

void optimizeModule(llvm::Module& M, llvm::TargetMachine& TM, const std::string& optLevel,
                    const std::optional<llvm::PGOOptions>& pgo) {
  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;
  // ADR-213: forward the TargetMachine so buildPerModuleDefaultPipeline
  // registers target-specific TargetIRAnalysis (SIMD features). Without TM,
  // the default TTI reports zero vector registers and the loop vectorizer
  // silently skips every loop.
  // ADR-225: PGOOptions ride the PassBuilder constructor (IRInstr/IRUse);
  // nullopt is byte-identical to the old no-PGO pipeline.
  llvm::PassBuilder PB(&TM, llvm::PipelineTuningOptions(), pgo);
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
      "pli_mutex_init_recursive",
  };
  // The tasking model (ADR-234) adds pli_plat_getenv, pli_plat_cpu_count,
  // pli_plat_now_ms/us, pli_plat_page_size and the fiber-stack allocator.
  // They all carry the pli_plat_ prefix, which the check below covers.
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

bool emitLTOObject(llvm::Module& M, llvm::TargetMachine& TM, llvm::raw_pwrite_stream& out,
                   bool thin, const std::string& optLevel, std::string& err) {
  // Run the LTO pre-link pipeline (mirrors clang -flto): per-module LTO phase
  // plus, for ThinLTO, the summary the linker needs. A bare ThinLTO module
  // flag with no summary mislinks (silent empty link / backend crash).
  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;
  llvm::PassBuilder PB(&TM);
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  llvm::ModulePassManager MPM = thin ? PB.buildThinLTOPreLinkDefaultPipeline(toOptLevel(optLevel))
                                     : PB.buildLTOPreLinkDefaultPipeline(toOptLevel(optLevel));
  MPM.run(M, MAM);
  // Write the bitcode object lld consumes natively; ThinLTO embeds the
  // module summary the linker needs (FullLTO carries the whole module).
  if (thin) {
    const llvm::ModuleSummaryIndex& index = MAM.getResult<llvm::ModuleSummaryIndexAnalysis>(M);
    llvm::WriteBitcodeToFile(M, out, /*ShouldPreserveUseListOrder=*/false, &index);
  } else {
    llvm::WriteBitcodeToFile(M, out);
  }
  out.flush();
  // Write errors surface at open time (caller's error_code) and close; the
  // stream itself exposes none. Nothing fallible remains here.
  (void)err;
  return true;
}

namespace {
// lldMain (not the per-driver `link` entry points) is LLD's documented
// re-entry-safe API: it destroys the linker context after every run, which a
// retry (ADR-280) needs. It selects the driver from argv[0], so the canonical
// name for the object format replaces our generic "ld.lld". MinGW keeps
// "ld.lld": its `-m i386pep` makes the dispatcher pick the MinGW driver.
const lld::DriverDef kLldDrivers[] = {
    {lld::Gnu, &lld::elf::link},
    {lld::Darwin, &lld::macho::link},
    {lld::WinLink, &lld::coff::link},
    {lld::MinGW, &lld::mingw::link},
};

bool runLld(llvm::ArrayRef<const char*> args, const char* driver, llvm::raw_ostream& out,
            llvm::raw_ostream& errsRef, bool* canRetry) {
  std::vector<const char*> argv(args.begin(), args.end());
  argv[0] = driver;
  lld::Result r = lld::lldMain(argv, out, errsRef, kLldDrivers);
  if (canRetry)
    *canRetry = r.canRunAgain;
  return r.retCode == 0;
}
} // namespace

bool linkExecutable(llvm::ArrayRef<const char*> args, int objFmt, std::string& err,
                    llvm::raw_ostream* outs, llvm::raw_ostream* errs, bool* canRetry) {
#if !PLI_HAVE_LLD
#error "in-process lld is mandatory (ADR-225): configure against an LLVM that ships lld"
#endif
  // Optional streams default to real stdout/stderr so callers that don't care
  // see normal output; the MachO two-pass path captures the first pass.
  llvm::raw_ostream& out = outs ? *outs : llvm::outs();
  llvm::raw_ostream& errsRef = errs ? *errs : llvm::errs();
  const char* driver = "ld.lld";
  switch (static_cast<llvm::Triple::ObjectFormatType>(objFmt)) {
  case llvm::Triple::ELF:
    break;
  case llvm::Triple::MachO:
    driver = "ld64.lld";
    break;
  case llvm::Triple::COFF:
    driver = "lld-link";
    break;
  default:
    err = "unsupported object format";
    return false;
  }
  return runLld(args, driver, out, errsRef, canRetry);
}

// ADR-227: the MinGW driver links GNU mingw-w64 objects (crt2.o,
// libmingw32.a with its .refptr.__image_base__ GNU-isms) that the COFF
// driver rejects. Same Common-driver argv contract as linkExecutable; its
// `-m i386pep` is what routes lldMain to the MinGW driver.
bool linkMinGWExecutable(llvm::ArrayRef<const char*> args, std::string& err) {
#if !PLI_HAVE_LLD
#error "in-process lld is mandatory (ADR-225): configure against an LLVM that ships lld"
#endif
  (void)err;
  return runLld(args, "ld.lld", llvm::outs(), llvm::errs(), nullptr);
}
} // namespace pli
