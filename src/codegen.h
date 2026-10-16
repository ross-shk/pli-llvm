// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// codegen.h — in-process codegen + linking (LLVM MC + lld C++ API).
//
// Runtime-once rule (review §3, normative): user objects emit pure (no
// runtime). The embedded pli_runtime.bc materialises once per final link.
#pragma once
#include <memory>
#include <optional>
#include <string>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/PGOOptions.h"
#include "llvm/TargetParser/Triple.h"

namespace llvm {
class Function;
class Module;
class LLVMContext;
class TargetMachine;
class raw_pwrite_stream;
} // namespace llvm

namespace pli {
// Re-apply the audited P0 side-table facts (kRuntimeAttrs in irgen.cpp) to
// one linked runtime definition whose declaration-time facts were lost at
// link time. Unknown names (user functions) are left alone.
void applyRuntimeFnAttrs(llvm::Function& f);
// Run the IR optimization pipeline for the driver `-O` level (`-O0`..`-O3`,
// `-Os`). The clang backend this replaces optimized at the same level; without
// this the in-process path would emit unoptimized IR (behaviour + perf drift).
// ADR-213: the TargetMachine is forwarded to the PassBuilder so the pipeline
// uses target-specific TargetIRAnalysis (NEON/SSE/AVX) — without it the loop
// vectorizer sees a default target with zero vector registers.
// ADR-225: `pgo` carries in-process PGO (IRInstr/IRUse); nullopt means no PGO.
void optimizeModule(llvm::Module& M, llvm::TargetMachine& TM, const std::string& optLevel,
                    const std::optional<llvm::PGOOptions>& pgo = std::nullopt);
// Emit a fully-relocatable object for `M` in memory (integrated assembler).
bool emitObject(llvm::Module& M, llvm::TargetMachine& TM, llvm::raw_pwrite_stream& out,
                std::string& err);
// Emit `M` as an LTO bitcode object for in-process lld (`--flto=thin|full`):
// runs the matching LTO pre-link pipeline (mirrors clang -flto) and writes
// bitcode with the module summary lld needs. lld runs its built-in LTO at
// link time, no plugin, no clang.
bool emitLTOObject(llvm::Module& M, llvm::TargetMachine& TM, llvm::raw_pwrite_stream& out,
                   bool thin, const std::string& optLevel, std::string& err);
// Single-module fast path only: link the embedded runtime into `M` at BC
// level (LinkOnlyNeeded). Never for `-c`, never per-module of multi-input.
bool linkEmbeddedLibPLI(llvm::Module& M, const llvm::Triple& targetTriple, std::string& err,
                        const std::string& platformBc = "");
// Split runtime: load the platform TU — a custom --rt-platform file or the
// embedded pli_rtplat.bc for the target — triple-checked and stripped. Null
// on failure (err set); null with err untouched when no embedded platform
// blob exists and no file was given (caller decides).
std::unique_ptr<llvm::Module> loadPlatformModule(llvm::LLVMContext& ctx,
                                                 const llvm::Triple& targetTriple,
                                                 const std::string& platformBc, std::string& err);
// True for every symbol the portable blob imports from the platform TU:
// pli_plat_* plus the threading backend names. Used to diagnose an
// incomplete custom platform file by name instead of a bare link error.
bool isPlatformSeamSymbol(const std::string& name);
// Diagnose every still-undefined seam symbol in `M` (an incomplete custom
// platform); empty string when the seam is complete. The default platform
// blob passes this by construction.
std::string checkPlatformSeamComplete(const llvm::Module& M);
// Multi-input path: parse the embedded bundle into its own module.
std::unique_ptr<llvm::Module> parseEmbeddedRuntime(const llvm::Triple& targetTriple,
                                                   llvm::LLVMContext& ctx, std::string& err);
// Emit the embedded runtime as its own object (one lld input among others).
// platformBc selects a custom platform TU merged in place of the default.
bool emitRuntimeObject(llvm::TargetMachine& TM, const llvm::Triple& targetTriple,
                       llvm::raw_pwrite_stream& out, std::string& err,
                       const std::string& platformBc = "");
// Link object files + system libs into an executable in-process (needs lld).
bool linkExecutable(llvm::ArrayRef<const char*> args, int objFmt, std::string& err);
} // namespace pli
