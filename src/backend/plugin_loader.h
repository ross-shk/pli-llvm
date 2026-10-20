// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// plugin_loader.h — load a code-generation backend plugin (ADR-235).
//
// A plugin is a shared object exporting pli_backend_plugin_v1 (see
// plugin_abi.h). The loader owns the handle, performs the ABI/LLVM
// handshake, and runs the transform hook. Everything the plugin returns is
// text: the compiler parses and verifies it, so a plugin cannot smuggle
// unverified IR into the pipeline.
#pragma once
#include <string>
#include <vector>

namespace pli {
// One loaded backend.
struct LoadedBackend {
  std::string name;
  std::string description;
  std::string path;
  const void* handle = nullptr; // the shared object handle
  const void* plugin = nullptr; // the PliBackendPlugin descriptor
};

// Load a plugin from `path`. Returns false with a diagnostic in `err` when
// the file cannot be loaded, the entry symbol is missing, or the handshake
// fails (ABI version, struct size, LLVM version).
bool loadBackendPlugin(const std::string& path, std::string& err);
// The plugins loaded so far, in load order.
const std::vector<LoadedBackend>& loadedBackends();
// The names of the loaded plugins, for diagnostics.
std::string backendNames();
// Run the transform hook of the backend named `name`. `moduleIr` is the
// unit's IR; `outIr` receives the IR the plugin returned. False with a
// diagnostic in `err` when the plugin is unknown or fails.
bool runBackendTransform(const std::string& name, const std::string& moduleIr,
                         const std::string& targetTriple, const std::string& optLevel,
                         bool isMainUnit, std::string& outIr, std::string& err);
} // namespace pli
