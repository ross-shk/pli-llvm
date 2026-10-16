// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// pgo.h — in-process PGO (profile-guided optimization), no clang dependency.
//
// ADR-225: --fprofile-generate/--fprofile-use are served in-process via LLVM's
// PassBuilder PGOOptions + InstrProfReader (raw or indexed auto-detect) +
// the compiler-rt profile runtime linked by in-process lld.
#pragma once
#include <optional>
#include <set>
#include <string>

namespace llvm {
class Module;
class Triple;
struct PGOOptions;
} // namespace llvm

namespace pli {
enum class PgoMode { None, Generate, Use };

// Build the PassBuilder PGOOptions for `mode`. Generate instruments the
// module (counters written to default.profraw at run time); Use consumes
// `profilePath` (raw .profraw or indexed .profdata — auto-detected).
std::optional<llvm::PGOOptions> makePgoOptions(PgoMode mode, const std::string& profilePath = "");

// Validate a --fprofile-use= file against the module about to be optimized:
// must parse (raw or indexed) and carry IR-level counters for at least one
// function in `M`. False with a human diagnostic in `err` otherwise (stale or
// corrupt profile instead of a silent miscompile).
bool validateUseProfile(const std::string& profilePath, const llvm::Module& M,
                        const std::set<std::string>& runtimeNames, std::string& err);

// Function names defined by the embedded runtime blob for `triple` (used to
// exclude runtime overlap from stale-profile validation). Empty on failure
// (caller proceeds strict: every record must then match the module).
std::set<std::string> runtimeFuncNames(const llvm::Triple& triple);

// Locate the compiler-rt profile runtime archive for the instrumented link:
// <exeDir>/clang_rt.profile-<arch>.a first (shipped in the release archive),
// then the toolchain resource layout. Empty when absent (caller diagnoses).
std::string profileRuntimeLib(const llvm::Triple& triple, const std::string& exeDir);
} // namespace pli
