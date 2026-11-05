// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// memfd.cpp — anonymous-fd blob seam for the in-process lld link.
#include "memfd.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>

#if !defined(_WIN32)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace pli {

namespace {

std::map<std::string, int>& memFdTable() {
  static std::map<std::string, int> table;
  return table;
}

} // namespace

std::string createMemBlob(llvm::StringRef bytes, std::string& err) {
#if defined(__APPLE__)
  // macOS: no memfd_create. mkstemp + immediate unlink leaves an unlinked
  // inode held open by the fd; /dev/fd/N re-opens it for lld's MemoryBuffer.
  std::string tmpl = fs::temp_directory_path().string() + "/pli-mem-XXXXXX";
  int fd = mkstemp(&tmpl[0]);
  if (fd < 0) {
    err = "mkstemp failed";
    return "";
  }
  ssize_t written = write(fd, bytes.data(), bytes.size());
  if (written < 0 || static_cast<size_t>(written) != bytes.size()) {
    close(fd);
    err = "write to mem blob failed";
    return "";
  }
  if (unlink(tmpl.c_str()) != 0) {
    close(fd);
    err = "unlink mem blob failed";
    return "";
  }
  std::string path = "/dev/fd/" + std::to_string(fd);
  memFdTable()[path] = fd;
  return path;
#elif defined(__linux__)
  // Linux: memfd_create gives a truly anonymous, unnamed fd; /proc/self/fd/N
  // is the openable path lld reads through.
  int fd = memfd_create("pli-mem-blob", MFD_CLOEXEC);
  if (fd < 0) {
    err = "memfd_create failed";
    return "";
  }
  ssize_t written = write(fd, bytes.data(), bytes.size());
  if (written < 0 || static_cast<size_t>(written) != bytes.size()) {
    close(fd);
    err = "write to mem blob failed";
    return "";
  }
  if (lseek(fd, 0, SEEK_SET) < 0) {
    close(fd);
    err = "lseek mem blob failed";
    return "";
  }
  std::string path = "/proc/self/fd/" + std::to_string(fd);
  memFdTable()[path] = fd;
  return path;
#elif defined(_WIN32)
  // Windows has no anonymous-fd path that lld can reopen by name, so fall back
  // to a uniquely named temp file; closeMemBlob removes it after the link.
  std::error_code ec;
  fs::path dir = fs::temp_directory_path(ec);
  if (ec) {
    err = "temp_directory_path failed";
    return "";
  }
  llvm::SmallString<128> path;
  std::string model = (dir / "pli-mem-%%%%%%").string();
  ec = llvm::sys::fs::createUniqueFile(model, path);
  if (ec) {
    err = "create temp blob failed: " + ec.message();
    return "";
  }
  std::ofstream os(std::string(path), std::ios::binary | std::ios::trunc);
  os.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  os.close();
  if (!os) {
    llvm::sys::fs::remove(path);
    err = "write to mem blob failed";
    return "";
  }
  std::string p(path.str());
  memFdTable()[p] = -1;
  return p;
#else
  err = "in-memory blob not supported on this platform";
  return "";
#endif
}

void closeMemBlob(const std::string& path) {
  auto it = memFdTable().find(path);
  if (it == memFdTable().end())
    return;
  int fd = it->second;
  memFdTable().erase(it);
#if defined(_WIN32)
  // The blob is a temp file: delete it now that lld no longer needs it.
  (void)fd;
  std::error_code ec;
  fs::remove(fs::path(path), ec);
#else
  if (fd >= 0)
    ::close(fd);
#endif
}

} // namespace pli
