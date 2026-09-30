// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM
// codegen.h — in-process codegen + linking (LLVM MC + lld C++ API).
//
// Runtime-once rule (review §3, normative): user objects emit pure (no
// runtime). The embedded libpli.bc materialises once per final link.
#pragma once
#include <memory>
#include <string>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/TargetParser/Triple.h"

namespace llvm {
class Module;
class LLVMContext;
class TargetMachine;
class raw_pwrite_stream;
} // namespace llvm

namespace plic {
// Run the IR optimization pipeline for the driver `-O` level (`-O0`..`-O3`,
// `-Os`). The clang backend this replaces optimized at the same level; without
// this the in-process path would emit unoptimized IR (behaviour + perf drift).
void optimizeModule(llvm::Module& M, const std::string& optLevel);
// Emit a fully-relocatable object for `M` in memory (integrated assembler).
bool emitObject(llvm::Module& M, llvm::TargetMachine& TM, llvm::raw_pwrite_stream& out,
                std::string& err);
// Single-module fast path only: link the embedded runtime into `M` at BC
// level (LinkOnlyNeeded). Never for `-c`, never per-module of multi-input.
bool linkEmbeddedLibPLI(llvm::Module& M, const llvm::Triple& targetTriple, std::string& err);
// Multi-input path: parse the embedded bundle into its own module.
std::unique_ptr<llvm::Module> parseEmbeddedRuntime(const llvm::Triple& targetTriple, llvm::LLVMContext& ctx, std::string& err);
// Emit the embedded runtime as its own object (one lld input among others).
bool emitRuntimeObject(llvm::TargetMachine& TM, const llvm::Triple& targetTriple, llvm::raw_pwrite_stream& out, std::string& err);
// Link object files + system libs into an executable in-process (needs lld).
bool linkExecutable(llvm::ArrayRef<const char*> args, int objFmt, std::string& err);
} // namespace plic
