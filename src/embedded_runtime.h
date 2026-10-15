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

#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/TargetParser/Triple.h"

namespace pli {
// The embedded pli_runtime bitcode bundle as a bitcode buffer (no OS lookup).
llvm::MemoryBufferRef getEmbeddedLibPLI();
// Byte size of the embedded bundle (reported by `pli version`).
size_t embeddedLibPLISize();

// Per-target runtime blob for cross-compilation.
struct RuntimeBlob {
  const unsigned char* data;
  size_t len;
  std::string target;
};
// Returns all embedded runtime blobs (one per supported target).
std::vector<RuntimeBlob> getEmbeddedRuntimeBlobs();
// Selects the appropriate runtime blob for the given target triple.
// Falls back to the default (host) blob if no exact match.
const RuntimeBlob* selectRuntimeForTarget(const llvm::Triple& t);
// Default platform blob (host pli_rtplat.bc) and per-target platform blobs.
// selectPlatformForTarget mirrors selectRuntimeForTarget, including the host
// fallback; it returns nullptr only when no platform blob was embedded.
std::vector<RuntimeBlob> getEmbeddedPlatformBlobs();
const RuntimeBlob* selectPlatformForTarget(const llvm::Triple& t);
} // namespace pli
