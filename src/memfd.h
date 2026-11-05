// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// memfd.h — anonymous-fd blob seam for the in-process lld link.
//
// Turns a byte blob into an openable path, so lld can read objects / archives
// / .tbd stubs through /proc/self/fd/N (Linux) or /dev/fd/N (macOS
// mkstemp+unlink) instead of a real temp path. Zero bytes are persisted; the
// fd keeps the unlinked inode alive for the whole link. Windows has no
// anonymous-fd path lld can reopen, so it falls back to a uniquely named temp
// file that closeMemBlob removes.
#pragma once
#include <string>

namespace llvm {
class StringRef;
}

namespace pli {
// Create a blob path holding `bytes` (anonymous fd where available).
// Returns a path (e.g. "/proc/self/fd/N" or "/dev/fd/N") that lld can open, or
// "" on failure (err set). The returned path is valid until closeMemBlob(path).
// One blob per path; the table is owned by the link scope in main().
std::string createMemBlob(llvm::StringRef bytes, std::string& err);
// Release the blob backing `path` (best-effort; closes the fd and, on
// Windows where the blob is a temp file, removes it).
void closeMemBlob(const std::string& path);
} // namespace pli
