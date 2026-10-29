// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// sanitize.h — AddressSanitizer instrumentation for the emitted module (B13).
#pragma once
#include <string>

namespace llvm {
class Module;
class Triple;
} // namespace llvm

namespace pli {
// ADR-341: instrument `M`'s memory accesses with ASan checks. Runs on the
// user module before the precompiled runtime blob is merged, so the runtime
// stays uninstrumented (B0 measures the runtime separately).
void instrumentAddressSanitizer(llvm::Module& M);

// Locate compiler-rt's AddressSanitizer runtime for `triple`: exe-relative
// first (release layout), then the toolchain resource dir. Empty when absent
// (Windows is not served: the MinGW link takes no MSVC-style runtime).
std::string addressSanitizerRuntime(const llvm::Triple& triple, const std::string& exeDir);
} // namespace pli
