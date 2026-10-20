/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* pli-llvm — pluggable code-generation backend, ABI v1 (ADR-235).
 *
 * A backend plugin is a shared object the compiler loads at run time
 * (dlopen / LoadLibrary). The boundary is deliberately narrow and stable:
 *
 *   - no C++ and no LLVM types cross it, so a plugin built against another
 *     compiler revision still loads (the handshake below rejects it);
 *   - the plugin sees the unit's LLVM IR as text and returns the IR it wants
 *     compiled, also as text;
 *   - the compiler re-parses and verifies the result, so invalid IR never
 *     reaches the optimizer or the object writer.
 *
 * The hook runs after the runtime bitcode is merged and before the
 * optimization pipeline, once per compiled unit.
 *
 * A plugin is arbitrary code running inside the compiler process: load only
 * plugins you trust.
 */
#ifndef PLI_BACKEND_ABI_H
#define PLI_BACKEND_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLI_BACKEND_ABI_VERSION 1u
/* The exported symbol: its identifier and its name (for dlsym/GetProcAddress). */
#define PLI_BACKEND_ENTRY_SYMBOL pli_backend_plugin_v1
#define PLI_BACKEND_ENTRY_SYMBOL_NAME "pli_backend_plugin_v1"

#if defined(_WIN32)
#define PLI_BACKEND_EXPORT __declspec(dllexport)
#else
#define PLI_BACKEND_EXPORT __attribute__((visibility("default")))
#endif

/* What the compiler tells the plugin about the unit being compiled. */
typedef struct PliBackendOptions {
  uint32_t size;             /* sizeof(PliBackendOptions): forward compatible */
  const char* target_triple; /* e.g. "x86_64-unknown-linux-gnu" */
  const char* opt_level;     /* "-O0" .. "-O3" */
  int is_main_unit;          /* 1 when the unit carries the runtime blob */
} PliBackendOptions;

/* The plugin descriptor: the single entry point a plugin must export. */
typedef struct PliBackendPlugin {
  uint32_t abi_version;     /* must equal PLI_BACKEND_ABI_VERSION */
  uint32_t struct_size;     /* must equal sizeof(PliBackendPlugin) */
  const char* name;         /* unique backend name, e.g. "acme" */
  const char* description;  /* one line, shown by `pli version` */
  const char* llvm_version; /* must equal the compiler's LLVM version */

  /* Transform hook. `module_ir` is the unit's LLVM IR as text; the plugin
   * returns the IR it wants compiled (a buffer the plugin owns and releases
   * through `release`). Returning NULL is a failure and `err` must then
   * point at a message the compiler prints. */
  char* (*transform_module)(const char* module_ir, const PliBackendOptions* options, char** err);

  /* Release a buffer returned by transform_module. */
  void (*release)(char* buffer);
} PliBackendPlugin;

/* The one symbol a plugin exports. */
PLI_BACKEND_EXPORT const PliBackendPlugin* PLI_BACKEND_ENTRY_SYMBOL(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PLI_BACKEND_ABI_H */
