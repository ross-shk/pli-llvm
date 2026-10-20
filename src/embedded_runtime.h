// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// embedded_runtime.h — the pli_runtime bitcode bundle embedded as data.
//
// Go/Zig embed their runtime/stdlib; pli embeds `pli_runtime.bc` as a generated
// byte array (see scripts/gen_embed.py), so no libpli_runtime.a/.bc ships at runtime.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

#include "llvm/TargetParser/Triple.h"

namespace pli {
// Byte size of the embedded bundle (reported by `pli version`).
size_t embeddedLibPLISize();

// Per-target runtime blob for cross-compilation.
struct RuntimeBlob {
  const unsigned char* data;
  size_t len;
  std::string target;
};
// Returns all embedded runtime blobs (the host plus one per cross target).
std::vector<RuntimeBlob> getEmbeddedRuntimeBlobs();
// Selects the runtime blob for the given target triple: an exact triple match
// first, then the same ABI family (arch + OS + Windows environment — see
// targetKey). A blob is never used for a different family, and an empty blob
// (a build without PLI_CROSS_BITCODE) is never selected.
const RuntimeBlob* selectRuntimeForTarget(const llvm::Triple& t);
// The triples this build embeds a runtime for (diagnostics, `pli version`).
std::vector<std::string> embeddedRuntimeTargets();
// Default platform blob (host pli_rtplat.bc) and per-target platform blobs.
// selectPlatformForTarget mirrors selectRuntimeForTarget, including the family
// scoping (ADR-234): a platform blob never crosses an ABI family.
std::vector<RuntimeBlob> getEmbeddedPlatformBlobs();
const RuntimeBlob* selectPlatformForTarget(const llvm::Triple& t);
} // namespace pli
