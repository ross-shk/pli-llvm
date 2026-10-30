// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// hostcrt.cpp — embedded link sysroot materialization for the in-process lld link.
//
// The link sysroot is EMBEDDED in pli(.exe) — release archives hold only the
// binary. At link time the driver materializes these blobs to a temp dir.
// No host CRT probing, no SDK lookup, no subprocesses (ADR-225).
#include "hostcrt.h"

#include "embedded_sysroot.h"
#include "llvm/Support/FileSystem.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace pli {

EmbeddedSysroot materializeEmbeddedSysroot() {
  EmbeddedSysroot out;
  size_t n = 0;
  const SysrootFile* files = embeddedSysrootFiles(&n);
  const char* setName = embeddedSysrootName();
  if (!files || n == 0 || !setName || !*setName)
    return out;
  std::error_code ec;
  fs::path base = fs::temp_directory_path(ec);
  if (ec)
    return out;
  // One link per invocation: a unique directory under the temp dir. The name
  // is random and the directory is created atomically (create_directory fails
  // if the name exists), so neither a stale crash leftover nor a name planted
  // in the temp dir can be mistaken for ours. A predictable
  // "<tmp>/pli-sysroot-<pid>" path let a planted symlink redirect the writes
  // below. (Relative paths inside the set, e.g. sdk's usr/lib/..., are
  // recreated below.)
  fs::path dir;
  for (int attempt = 0; attempt < 8 && dir.empty(); ++attempt) {
    llvm::SmallString<128> candBuf;
    llvm::sys::fs::createUniquePath((base / "pli-sysroot-%%%%%%").string(), candBuf, false);
    fs::path cand(candBuf.str().str());
    std::error_code cec;
    // create_directory returns false when the name is already taken.
    if (!fs::create_directory(cand, cec))
      continue; // the name was taken: try another
    dir = cand;
  }
  if (dir.empty())
    return out;
  auto fail = [&]() {
    std::error_code cec;
    fs::remove_all(dir, cec);
    return EmbeddedSysroot();
  };
  for (size_t i = 0; i < n; ++i) {
    fs::path p = dir / files[i].path;
    fs::create_directories(p.parent_path(), ec);
    if (ec)
      return fail();
    std::ofstream os(p, std::ios::binary | std::ios::trunc);
    if (!os)
      return fail();
    os.write(reinterpret_cast<const char*>(files[i].data),
             static_cast<std::streamsize>(files[i].len));
    os.close();
    if (!os)
      return fail();
  }
  out.name = setName;
  out.dir = dir.string();
  return out;
}

void cleanupMaterializedSysroot(const std::string& dir) {
  if (dir.empty())
    return;
  // Only remove what materializeEmbeddedSysroot created: a directory directly
  // under the temp dir whose name starts with the marker prefix. A substring
  // test would also match a path like "/tmp/evil-pli-sysroot-x".
  fs::path p(dir);
  std::error_code ec;
  fs::path base = fs::temp_directory_path(ec);
  if (ec)
    return;
  // The path comparison is element-wise, but the temp dir path may carry a
  // trailing separator ("/var/.../T/"), which makes the native strings
  // differ. Compare without it.
  auto withoutTrailingSep = [](const fs::path& q) {
    std::string s = q.string();
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\'))
      s.pop_back();
    return s;
  };
  if (withoutTrailingSep(p.parent_path()) != withoutTrailingSep(base))
    return;
  if (p.filename().string().rfind("pli-sysroot-", 0) != 0)
    return;
  fs::remove_all(p, ec);
}

} // namespace pli
