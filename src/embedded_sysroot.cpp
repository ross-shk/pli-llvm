// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// embedded_sysroot.cpp — expose the embedded link sysroot table.
#include "embedded_sysroot.h"

// Generated manifest (build/embedded_sysroot.inc via
// scripts/gen_sysroot_embed.py): extern array declarations plus the
// kEmbeddedSysrootFiles table. Empty table when this build embedded none.
#include "embedded_sysroot.inc"

namespace pli {

const SysrootFile* embeddedSysrootFiles(size_t* n) {
  *n = kEmbeddedSysrootFileCount;
  return kEmbeddedSysrootFileCount > 0 ? kEmbeddedSysrootFiles : nullptr;
}

const char* embeddedSysrootName() { return kEmbeddedSysrootName; }

size_t embeddedSysrootSize() {
  size_t total = 0;
  for (size_t i = 0; i < kEmbeddedSysrootFileCount; ++i)
    total += kEmbeddedSysrootFiles[i].len;
  return total;
}

} // namespace pli
