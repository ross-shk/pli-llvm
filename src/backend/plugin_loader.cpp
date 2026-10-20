// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// plugin_loader.cpp — load a code-generation backend plugin (ADR-235).
#include "plugin_loader.h"

#include "plugin_abi.h"

#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace pli {
namespace {

std::vector<LoadedBackend>& registry() {
  static std::vector<LoadedBackend> backends;
  return backends;
}

#ifdef _WIN32
using LibHandle = HMODULE;
LibHandle openLib(const std::string& path) { return LoadLibraryA(path.c_str()); }
void* symOf(LibHandle h, const char* name) { return (void*)GetProcAddress(h, name); }
void closeLib(LibHandle h) { FreeLibrary(h); }
std::string libError() {
  DWORD code = GetLastError();
  char buf[256];
  FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buf,
                 sizeof(buf), nullptr);
  return buf;
}
#else
using LibHandle = void*;
LibHandle openLib(const std::string& path) { return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL); }
void* symOf(LibHandle h, const char* name) { return dlsym(h, name); }
void closeLib(LibHandle h) { dlclose(h); }
std::string libError() {
  const char* e = dlerror();
  return e ? e : "unknown error";
}
#endif

} // namespace

bool loadBackendPlugin(const std::string& path, std::string& err) {
  LibHandle handle = openLib(path);
  if (!handle) {
    err = "cannot load backend plugin '" + path + "': " + libError();
    return false;
  }
  void* sym = symOf(handle, PLI_BACKEND_ENTRY_SYMBOL_NAME);
  if (!sym) {
    err = "backend plugin '" + path + "' does not export " PLI_BACKEND_ENTRY_SYMBOL_NAME;
    closeLib(handle);
    return false;
  }
  const PliBackendPlugin* plugin = reinterpret_cast<const PliBackendPlugin* (*)()>(sym)();
  if (!plugin) {
    err = "backend plugin '" + path + "' returned no descriptor";
    closeLib(handle);
    return false;
  }
  // The handshake: the ABI version, the descriptor size and the LLVM
  // version must all match, or the plugin is not usable here.
  if (plugin->abi_version != PLI_BACKEND_ABI_VERSION) {
    err = "backend plugin '" + path + "' speaks ABI version " +
          std::to_string(plugin->abi_version) + " (this compiler speaks " +
          std::to_string(PLI_BACKEND_ABI_VERSION) + ")";
    closeLib(handle);
    return false;
  }
  if (plugin->struct_size != sizeof(PliBackendPlugin)) {
    err = "backend plugin '" + path + "' has a descriptor of " +
          std::to_string(plugin->struct_size) + " bytes (this compiler uses " +
          std::to_string(sizeof(PliBackendPlugin)) + ")";
    closeLib(handle);
    return false;
  }
  if (!plugin->llvm_version || std::strcmp(plugin->llvm_version, PLI_LLVM_VERSION) != 0) {
    err = "backend plugin '" + path + "' was built against LLVM " +
          (plugin->llvm_version ? plugin->llvm_version : "unknown") +
          " (this compiler uses " PLI_LLVM_VERSION ")";
    closeLib(handle);
    return false;
  }
  if (!plugin->transform_module || !plugin->release) {
    err = "backend plugin '" + path + "' is missing the transform hooks";
    closeLib(handle);
    return false;
  }
  LoadedBackend backend;
  backend.name = plugin->name ? plugin->name : "unnamed";
  backend.description = plugin->description ? plugin->description : "";
  backend.path = path;
  backend.handle = handle;
  backend.plugin = plugin;
  registry().push_back(backend);
  return true;
}

const std::vector<LoadedBackend>& loadedBackends() { return registry(); }

std::string backendNames() {
  std::string out;
  for (const LoadedBackend& b : registry()) {
    if (!out.empty())
      out += ", ";
    out += b.name;
  }
  return out;
}

bool runBackendTransform(const std::string& name, const std::string& moduleIr,
                         const std::string& targetTriple, const std::string& optLevel,
                         bool isMainUnit, std::string& outIr, std::string& err) {
  const LoadedBackend* found = nullptr;
  for (const LoadedBackend& b : registry())
    if (b.name == name)
      found = &b;
  if (!found) {
    err = "unknown backend '" + name + "'";
    return false;
  }
  const PliBackendPlugin* plugin = static_cast<const PliBackendPlugin*>(found->plugin);
  PliBackendOptions options;
  options.size = sizeof(options);
  options.target_triple = targetTriple.c_str();
  options.opt_level = optLevel.c_str();
  options.is_main_unit = isMainUnit ? 1 : 0;
  char* pluginErr = nullptr;
  char* result = plugin->transform_module(moduleIr.c_str(), &options, &pluginErr);
  if (!result) {
    err = "backend '" + name + "' failed";
    if (pluginErr && *pluginErr) {
      err += ": ";
      err += pluginErr;
    }
    return false;
  }
  outIr.assign(result);
  plugin->release(result);
  return true;
}

} // namespace pli
