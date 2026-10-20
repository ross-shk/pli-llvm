/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* A test backend plugin (ADR-235): it appends one global to the module it
 * receives, so the driver test can prove the plugin's transform reached the
 * compiled IR. The plugin is deliberately trivial — the point of the test is
 * the ABI handshake and the round trip, not the transform. */
#include "backend/plugin_abi.h"

#include <stdlib.h>
#include <string.h>

static char* transform(const char* module_ir, const PliBackendOptions* options, char** err) {
  (void)options;
  (void)err;
  const char* marker = "@pli_plugin_marker = global i32 42\n";
  size_t mlen = strlen(marker);
  size_t len = strlen(module_ir);
  /* Insert the global before the first function: a module-level global must
   * precede the functions and the trailing attribute groups. */
  const char* insert = module_ir;
  const char* p = module_ir;
  while (*p) {
    const char* eol = strchr(p, '\n');
    size_t llen = eol ? (size_t)(eol - p) : strlen(p);
    if ((llen >= 7 && strncmp(p, "define ", 7) == 0) ||
        (llen >= 8 && strncmp(p, "declare ", 8) == 0)) {
      insert = p;
      break;
    }
    if (!eol)
      break;
    p = eol + 1;
  }
  size_t head = (size_t)(insert - module_ir);
  char* out = (char*)malloc(len + mlen + 1);
  if (!out)
    return NULL;
  memcpy(out, module_ir, head);
  memcpy(out + head, marker, mlen);
  memcpy(out + head + mlen, insert, len - head);
  out[len + mlen] = '\0';
  return out;
}

static void release_buffer(char* buffer) { free(buffer); }

static const PliBackendPlugin kPlugin = {
    PLI_BACKEND_ABI_VERSION,
    sizeof(PliBackendPlugin),
    "test",
    "test plugin: appends a marker global",
    PLI_LLVM_VERSION_STRING,
    transform,
    release_buffer,
};

PLI_BACKEND_EXPORT const PliBackendPlugin* PLI_BACKEND_ENTRY_SYMBOL(void) { return &kPlugin; }
