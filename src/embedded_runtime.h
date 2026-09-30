// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM
// embedded_runtime.h — the libpli bitcode bundle embedded as data.
//
// Go/Zig embed their runtime/stdlib; plic embeds `libpli.bc` as a generated
// byte array (see scripts/gen_embed.py), so no libpli.a/.bc ships at runtime.
#pragma once
#include <cstddef>

#include "llvm/Support/MemoryBufferRef.h"

namespace plic {
// The embedded libpli bitcode bundle as a bitcode buffer (no OS lookup).
llvm::MemoryBufferRef getEmbeddedLibPLI();
// Byte size of the embedded bundle (reported by `plic version`).
size_t embeddedLibPLISize();
} // namespace plic
