// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM
// codegen.cpp — in-process object emission + runtime-once helpers + lld link.
#include "codegen.h"

#include "embedded_runtime.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Triple.h"

#if PLIC_HAVE_LLD
#include "lld/Common/Driver.h"
#endif

namespace plic {
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

bool linkEmbeddedLibPLI(llvm::Module& M, std::string& err) {
  auto buf = getEmbeddedLibPLI();
  // Parse against M's own context so Linker can merge directly.
  llvm::Expected<std::unique_ptr<llvm::Module>> parsed =
      llvm::parseBitcodeFile(buf, M.getContext());
  if (!parsed) {
    err = "cannot parse embedded libpli.bc";
    llvm::consumeError(parsed.takeError());
    return false;
  }
  // Pull in only referenced runtime functions (replaces ADR-079 dead-strip).
  if (llvm::Linker::linkModules(M, std::move(*parsed), llvm::Linker::LinkOnlyNeeded)) {
    err = "cannot link embedded libpli.bc";
    return false;
  }
  return true;
}

std::unique_ptr<llvm::Module> parseEmbeddedRuntime(llvm::LLVMContext& ctx, std::string& err) {
  auto buf = getEmbeddedLibPLI();
  auto modOrErr = llvm::parseBitcodeFile(buf, ctx);
  if (!modOrErr) {
    err = "cannot parse embedded libpli.bc";
    llvm::consumeError(modOrErr.takeError());
    return nullptr;
  }
  return std::move(*modOrErr);
}

bool emitRuntimeObject(llvm::TargetMachine& TM, llvm::raw_pwrite_stream& out, std::string& err) {
  llvm::LLVMContext ctx;
  auto rt = parseEmbeddedRuntime(ctx, err);
  if (!rt)
    return false;
  rt->setDataLayout(TM.createDataLayout());
  return emitObject(*rt, TM, out, err);
}

bool linkExecutable(llvm::ArrayRef<const char*> args, int objFmt, std::string& err) {
#if PLIC_HAVE_LLD
  // `args` are exactly the argv lld would receive (program name, then inputs).
  switch (static_cast<llvm::Triple::ObjectFormatType>(objFmt)) {
  case llvm::Triple::ELF:
    return lld::elf::link(args, false, llvm::outs(), llvm::errs());
  case llvm::Triple::MachO:
    return lld::mach_o::link(args, false, llvm::outs(), llvm::errs());
  case llvm::Triple::COFF:
    return lld::coff::link(args, false, llvm::outs(), llvm::errs());
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
