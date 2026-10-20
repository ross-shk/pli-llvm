// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// target.h — data-driven target knowledge (host now, additive later).
//
// Adding a cross target = adding a row + a bundled libc/startup lookup,
// without touching the pipeline.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "llvm/TargetParser/Triple.h"

namespace llvm {
class TargetMachine;
}

namespace pli {
struct TargetDesc {
  llvm::Triple::ObjectFormatType objFmt = llvm::Triple::UnknownObjectFormat;
  std::string defaultTriple;            // e.g. "arm64-apple-darwin25.6.0"
  std::string lldDriver;                // "elf" | "macho" | "coff"
  std::vector<std::string> systemLibs;  // -l... / .lib names
  std::vector<std::string> linkerFlags; // entry, platform_version, /subsystem
};
// Per-target descriptor (dispatches on triple's object format).
const TargetDesc& getTargetDesc(const llvm::Triple& t);
// Initialize all LLVM targets (not just native) for cross-compilation.
void initializeAllTargets();
// Normalised ABI family of a triple: "linux-x86_64", "windows-x86_64",
// "darwin-aarch64", ... Two triples with the same key share one runtime blob and
// one link sysroot (ADR-234). The Windows GNU (MinGW) and MSVC ABIs are
// separate families: their CRTs and calling conventions differ.
std::string targetKey(const llvm::Triple& t);
// Canonical arch name of a triple: "x86_64", "aarch64", ... LLVM's
// getArchName() echoes the spelling the user typed ("arm64" vs "aarch64" for
// the same Apple Silicon target), which would split one family in two.
std::string targetArchName(const llvm::Triple& t);
// The macOS deployment target of a Darwin triple, as a comparable number
// (15.0, 14.0, ...). A triple without a version gets the default (15.0),
// which matches the module triple IRGen stamps.
double macosDeploymentVersion(const llvm::Triple& t);
// The cross targets this compiler supports (scripts/cross_targets.txt).
const std::vector<std::string>& crossTargetTriples();
// True when `t` names the build host's family (the host link path applies).
bool isHostTarget(const llvm::Triple& t);
// What this build can emit for a target (ADR-236). The driver asks the
// profile one question — does this build have a backend for it? A target
// without one can only be produced as portable bitcode, which the user's own
// toolchain compiles and links. The other facts (an embedded runtime, a link
// sysroot, the host recipe) are asked where they are needed: by the runtime
// merge and by the link path (ADR-237).
struct TargetProfile {
  bool llvmBackend = false; // LLVM has a backend for this architecture
  bool portable = false;    // only portable bitcode can be emitted
};
// The profile of a target in this build.
TargetProfile describeTarget(const llvm::Triple& t);
// TargetMachine for the given triple; null on failure with `err` set.
// When *marchNative* is set, use the host CPU's feature set instead of
// "generic" (opt-in for local hot runs; CI/baselines always use generic).
std::unique_ptr<llvm::TargetMachine> createTargetMachine(const std::string& triple,
                                                         const std::string& optLevel,
                                                         std::string& err,
                                                         bool marchNative = false);
} // namespace pli
