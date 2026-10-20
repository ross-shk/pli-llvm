#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/cross_plugin.sh — the pluggable backend ABI (ADR-235).
#
# A backend plugin is a shared object exporting pli_backend_plugin_v1. The
# test builds one, loads it, and proves the plugin's transform reached the
# compiled IR; then it proves the loader rejects what it must reject: an
# unknown backend name, a missing file, and a plugin with a wrong ABI
# version.
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=tests/driver/out/cross_plugin
mkdir -p "$OUT"

ok=1
fail() { echo "FAIL: $1"; ok=0; }

cat > "$OUT.witness.pli" <<'EOF'
witness: procedure options(main);
   put skip list('PASS witness');
end witness;
EOF

# The plugin must report the compiler's LLVM version: take it from the
# compiler's own version line, so the handshake matches by construction.
LLVM_VER=$($PLI version 2>/dev/null | sed -n 's/.*(LLVM \([0-9.]*\),.*/\1/p')
[ -n "$LLVM_VER" ] || { echo "FAIL: cannot read the compiler's LLVM version"; exit 1; }

# The C compiler for the plugin: the same one the tests use elsewhere.
CC=${CC:-${CLANG:-clang}}
command -v "$CC" >/dev/null 2>&1 || CC=cc
command -v "$CC" >/dev/null 2>&1 || { echo "FAIL: no C compiler for the test plugin"; exit 1; }

SRC=tests/driver/plugin/test_plugin.c
case "$(uname -s)" in
  Darwin*) PLUGIN_FLAGS="-dynamiclib -fPIC"; PLUGIN="$OUT/test_plugin.dylib"; OUT_EXT=dylib ;;
  *) PLUGIN_FLAGS="-shared -fPIC"; PLUGIN="$OUT/test_plugin.so"; OUT_EXT=so ;;
esac

# $1 = output path, $2 = extra defines
build_plugin() {
  out=$1
  shift
  $CC $PLUGIN_FLAGS -I src -DPLI_LLVM_VERSION_STRING="\"$LLVM_VER\"" "$@" \
      "$SRC" -o "$out" 2>"$OUT.build.log" || {
    echo "FAIL: cannot build the test plugin"
    cat "$OUT.build.log"
    return 1
  }
}

build_plugin "$PLUGIN" || exit 1

# 1. The plugin's transform reaches the compiled IR.
if ! $PLI --backend-plugin "$PLUGIN" --backend test -emit-llvm "$OUT.witness.pli" \
        -o "$OUT.witness.ll" 2>"$OUT.err"; then
  fail "compile with the plugin"
  cat "$OUT.err"
elif ! grep -q "pli_plugin_marker" "$OUT.witness.ll"; then
  fail "the plugin's transform is missing from the emitted IR"
else
  # 2. The built-in backend is unaffected: no plugin, no marker.
  if $PLI -emit-llvm "$OUT.witness.pli" -o "$OUT.plain.ll" 2>/dev/null &&
     grep -q "pli_plugin_marker" "$OUT.plain.ll"; then
    fail "the marker appears without a plugin"
  fi
fi

# 3. `pli version` lists the loaded plugin with its description and its file.
if ! $PLI --backend-plugin "$PLUGIN" version 2>/dev/null | grep -q "test plugin"; then
  fail "the plugin's description is missing from 'pli version'"
fi

# 4. An unknown backend name is diagnosed, with the available ones listed.
if $PLI --backend-plugin "$PLUGIN" --backend nosuch -fsyntax-only "$OUT.witness.pli" \
     >"$OUT.err" 2>&1; then
  fail "an unknown backend was accepted"
elif ! grep -q "unknown backend" "$OUT.err"; then
  fail "the unknown-backend diagnostic is missing"
  cat "$OUT.err"
fi

# 5. A missing plugin file is diagnosed.
if $PLI --backend-plugin "$OUT/does-not-exist.so" -fsyntax-only "$OUT.witness.pli" \
     >"$OUT.err" 2>&1; then
  fail "a missing plugin was accepted"
elif ! grep -q "cannot load backend plugin" "$OUT.err"; then
  fail "the missing-plugin diagnostic is missing"
  cat "$OUT.err"
fi

# 6. A plugin with a wrong ABI version is rejected by the handshake.
cat > "$OUT/bad_abi.c" <<'EOF'
#include "backend/plugin_abi.h"
static char* transform(const char* ir, const PliBackendOptions* o, char** e) {
  (void)ir; (void)o; (void)e; return NULL;
}
static void release_buffer(char* b) { (void)b; }
static const PliBackendPlugin kPlugin = {
    PLI_BACKEND_ABI_VERSION + 1, sizeof(PliBackendPlugin), "bad", "bad abi",
    PLI_LLVM_VERSION_STRING, transform, release_buffer,
};
PLI_BACKEND_EXPORT const PliBackendPlugin* PLI_BACKEND_ENTRY_SYMBOL(void) { return &kPlugin; }
EOF
case "$(uname -s)" in
  Darwin*) BAD_FLAGS="-dynamiclib -fPIC" ;;
  *) BAD_FLAGS="-shared -fPIC" ;;
esac
if $CC $BAD_FLAGS -I src -DPLI_LLVM_VERSION_STRING="\"$LLVM_VER\"" "$OUT/bad_abi.c" \
     -o "$OUT/bad_abi.$OUT_EXT" 2>/dev/null; then
  if $PLI --backend-plugin "$OUT/bad_abi.$OUT_EXT" -fsyntax-only "$OUT.witness.pli" \
       >"$OUT.err" 2>&1; then
    fail "a plugin with a wrong ABI version was accepted"
  elif ! grep -q "ABI version" "$OUT.err"; then
    fail "the ABI-version diagnostic is missing"
    cat "$OUT.err"
  fi
fi

if [ "$ok" -eq 1 ]; then
  echo PASS
  exit 0
fi
exit 1
