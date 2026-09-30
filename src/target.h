// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM
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

namespace plic {
struct TargetDesc {
  llvm::Triple::ObjectFormatType objFmt = llvm::Triple::UnknownObjectFormat;
  std::string defaultTriple;            // e.g. "arm64-apple-darwin25.6.0"
  std::string lldDriver;                // "elf" | "macho" | "coff"
  std::vector<std::string> systemLibs;  // -l... / .lib names
  std::vector<std::string> linkerFlags; // entry, platform_version, /subsystem
};
// Host defaults today; cross rows added later (target != host is follow-up).
const TargetDesc& getTargetDesc(const llvm::Triple& t);
// TargetMachine for the host triple; null on failure with `err` set.
std::unique_ptr<llvm::TargetMachine>
createTargetMachine(const std::string& triple, const std::string& optLevel, std::string& err);
} // namespace plic
