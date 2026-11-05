// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// hostcrt.h — embedded link sysroot materialization for the in-process lld link.
//
// The link sysroot is EMBEDDED in pli(.exe) — release archives hold only the
// binary. At link time the driver materializes these blobs to a temp dir.
// No host CRT probing, no SDK lookup, no subprocesses (ADR-225).
#pragma once
#include <map>
#include <string>

namespace llvm {
class Triple;
}

namespace pli {
// The embedded sysroot materialized to a temp dir.
// name is one of "musl", "mingw", "sdk" (which link path it feeds); dir is
// empty when this build embedded none or the write failed — the caller fails
// the link with a clear error. Remove with cleanupMaterializedSysroot
// once the link is done (best-effort, like other temp link inputs).
struct EmbeddedSysroot {
  std::string name;
  std::string dir;
};
EmbeddedSysroot materializeEmbeddedSysroot();
void cleanupMaterializedSysroot(const std::string& dir);

// In-memory sysroot: each embedded blob mapped to an anonymous-fd path, so no
// named file is written for the link inputs (Windows falls back to temp files
// through createMemBlob). name is one of "musl", "mingw", "sdk"; empty when
// materialization fails — the caller falls back to materializeEmbeddedSysroot().
// files maps each logical path (e.g. "crt1.o", "usr/lib/libSystem.tbd") to its
// blob path. Close the blobs with cleanupMaterializedSysrootMem once linked.
struct EmbeddedSysrootMem {
  std::string name;
  std::map<std::string, std::string> files; // logicalPath -> memPath
};
EmbeddedSysrootMem materializeEmbeddedSysrootMem();
void cleanupMaterializedSysrootMem(EmbeddedSysrootMem& m);
} // namespace pli
