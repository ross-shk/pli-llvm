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
#else
#include <windows.h>
#include <io.h>
#endif

namespace fs = std::filesystem;

namespace pli {

namespace {

std::map<std::string, intptr_t>& memFdTable() {
  static std::map<std::string, intptr_t> table;
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
  // Windows: no anonymous-fd path lld can reopen. Use a temp file with
  // FILE_FLAG_DELETE_ON_CLOSE and share modes that allow lld to open it by
  // path while we hold the handle. The file is deleted when our handle closes.
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
  // Reopen with DELETE_ON_CLOSE and full sharing so lld can open the path.
  fs::path nativePath(path.c_str());
  HANDLE h = CreateFileW(
      nativePath.wstring().c_str(),
      GENERIC_WRITE | DELETE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
      nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    llvm::sys::fs::remove(path);
    err = "CreateFileW DELETE_ON_CLOSE failed: " + std::to_string(GetLastError());
    return "";
  }
  DWORD written = 0;
  if (!WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
      written != bytes.size()) {
    CloseHandle(h);
    err = "write to mem blob failed";
    return "";
  }
  // Keep handle open in the table; closeMemBlob will close it (triggering delete).
  std::string p(path.c_str());
  memFdTable()[p] = reinterpret_cast<intptr_t>(h);
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
  intptr_t handle = it->second;
  memFdTable().erase(it);
#if defined(_WIN32)
  // Close the handle; FILE_FLAG_DELETE_ON_CLOSE deletes the file.
  HANDLE h = reinterpret_cast<HANDLE>(handle);
  if (h != INVALID_HANDLE_VALUE)
    CloseHandle(h);
#else
  if (handle >= 0)
    ::close(static_cast<int>(handle));
#endif
}

} // namespace pli
