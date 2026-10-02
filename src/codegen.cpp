// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM
// codegen.cpp — in-process object emission + runtime-once helpers + lld link.
#include "codegen.h"

#include "embedded_runtime.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Triple.h"

#if PLIC_HAVE_LLD
#include "lld/Common/Driver.h"
// brew's lld ships only Common headers, so declare each driver's `link` entry
// point with the public macro (no lld/ELF/Driver.h in the install).
LLD_HAS_DRIVER(elf)
LLD_HAS_DRIVER(macho)
LLD_HAS_DRIVER(coff)
#endif

namespace plic {
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

void optimizeModule(llvm::Module& M, const std::string& optLevel) {
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

bool linkEmbeddedLibPLI(llvm::Module& M, const llvm::Triple& targetTriple, std::string& err) {
  const RuntimeBlob* blob = selectRuntimeForTarget(targetTriple);
  if (!blob) {
    err = "no embedded runtime for target: " + targetTriple.getTriple();
    return false;
  }
  // Parse against M's own context so Linker can merge directly.
  llvm::MemoryBufferRef buf(llvm::StringRef(reinterpret_cast<const char*>(blob->data), blob->len),
                            "libpli.bc");
  llvm::Expected<std::unique_ptr<llvm::Module>> parsed =
      llvm::parseBitcodeFile(buf, M.getContext());
  if (!parsed) {
    err = "cannot parse embedded libpli.bc";
    llvm::consumeError(parsed.takeError());
    return false;
  }
  // Strip per-function target attributes from the runtime functions before
  // linking. The bitcode is compiled for apple-m1 with probe-stack=__chkstk_darwin;
  // LLVM 23's AArch64 backend mis-handles these when a function has a large
  // stack frame ("Unsupported stack probing method"). Removing target-cpu,
  // target-features, and probe-stack forces codegen to use only the
  // TargetMachine's defaults. Mark them noinline so they stay as distinct,
  // linkable runtime symbols (the dead-strip driver test relies on this).
  for (auto& F : **parsed) {
    if (F.hasFnAttribute("target-cpu"))
      F.removeFnAttr("target-cpu");
    if (F.hasFnAttribute("target-features"))
      F.removeFnAttr("target-features");
    if (F.hasFnAttribute("probe-stack"))
      F.removeFnAttr("probe-stack");
    F.addFnAttr(llvm::Attribute::NoInline);
  }
  // Pull in only referenced runtime functions (replaces ADR-079 dead-strip).
  if (llvm::Linker::linkModules(M, std::move(*parsed), llvm::Linker::LinkOnlyNeeded)) {
    err = "cannot link embedded libpli.bc";
    return false;
  }
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
                            "libpli.bc");
  auto modOrErr = llvm::parseBitcodeFile(buf, ctx);
  if (!modOrErr) {
    err = "cannot parse embedded libpli.bc";
    llvm::consumeError(modOrErr.takeError());
    return nullptr;
  }
  return std::move(*modOrErr);
}

bool emitRuntimeObject(llvm::TargetMachine& TM, const llvm::Triple& targetTriple,
                       llvm::raw_pwrite_stream& out, std::string& err) {
  llvm::LLVMContext ctx;
  auto rt = parseEmbeddedRuntime(targetTriple, ctx, err);
  if (!rt)
    return false;
  // Strip the same per-function attributes that break LLVM 23 codegen
  // (see linkEmbeddedLibPLI): target-cpu/target-features/probe-stack from
  // the apple-m1-compiled runtime trigger "Unsupported stack probing method".
  for (auto& F : *rt) {
    if (F.hasFnAttribute("target-cpu"))
      F.removeFnAttr("target-cpu");
    if (F.hasFnAttribute("target-features"))
      F.removeFnAttr("target-features");
    if (F.hasFnAttribute("probe-stack"))
      F.removeFnAttr("probe-stack");
    F.addFnAttr(llvm::Attribute::NoInline);
  }
  rt->setDataLayout(TM.createDataLayout());
  return emitObject(*rt, TM, out, err);
}

bool linkExecutable(llvm::ArrayRef<const char*> args, int objFmt, std::string& err) {
#if PLIC_HAVE_LLD
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
} // namespace plic
