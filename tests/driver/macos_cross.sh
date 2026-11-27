#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/macos_cross.sh — macOS cross-compilation (ADR-396): link arm64
# vendored Darwin runtime blobs, and verify the object format/arch with
# llvm-readobj. Also checks -c / -shared / -emit-llvm and the strict blob
# fallback. The Darwin blob-independent checks (-emit-llvm triple preservation,
# strict no-host-fallback) run even before the blobs are bootstrapped; the link
# checks SKIP when the vendored blobs are absent (pre-bootstrap state).
set -u
PLI=${PLI:-./build/cmake/pli}
CLANG=${CLANG:-clang}
READOBJ=$(dirname "$CLANG")/llvm-readobj        # derive from CLANG dir
OUT=tests/driver/out/macos_cross
mkdir -p tests/driver/out "$OUT"
ok=1

cat > "$OUT.pli" <<'EOF'
  macos: proc options(main);
     declare x fixed bin(31);
     x = 21;
     put skip list('PASS macos_cross', x * 2);
  end macos;
EOF

# --- blob-independent: -emit-llvm must preserve the user's Apple triple ------
# (regression for the old forced arm64-apple-macosx15.0 in irgen.cpp)
$PLI --triple arm64-apple-macosx15.0 "$OUT.pli" -emit-llvm -o "$OUT.llarm" \
  || { echo "FAIL: arm64 -emit-llvm"; ok=0; }
grep -q 'target triple = "arm64-apple-macosx15.0"' "$OUT.llarm" \
  || { echo "FAIL: arm64 triple wrong"; ok=0; }

# --- blob-independent: Darwin link with no Darwin blob must fail cleanly, not
# silently fall back to the host runtime (embedding_runtime.cpp strict guard).
if [ ! -e platform/darwin-runtime/arm64/pli_runtime.bc ]; then
  if $PLI --triple arm64-apple-macosx15.0 "$OUT.pli" -c -o "$OUT.arm64.o" \
      2>"$OUT.noarch.err"; then
    echo "FAIL: Darwin -c succeeded despite no Darwin blob (silent host fallback)"
    ok=0
  else
    grep -qi "no embedded runtime" "$OUT.noarch.err" \
      || { echo "FAIL: Darwin-without-blob error not the strict diagnostic"; ok=0; }
  fi
fi

# --- blob-dependent link checks (SKIP pre-bootstrap) -------------------------
if [ ! -e platform/darwin-runtime/arm64/pli_runtime.bc ] \
   || [ ! -e platform/darwin-runtime/x86_64/pli_runtime.bc ]; then
  echo "SKIP: Darwin runtime blobs not vendored yet (bootstrap on macOS with scripts/gen_darwin_runtime.py)"
  [ "$ok" = 1 ] && echo "PASS macos_cross (blob-independent checks)"
  exit 0
fi

# 1. arm64 Darwin executable
$PLI --triple arm64-apple-macosx15.0 "$OUT.pli" -o "$OUT.arm64" \
  || { echo "FAIL: arm64 link"; ok=0; }
"$READOBJ" --file-headers "$OUT.arm64" | grep -q "Format: Mach-O arm64" \
  || { echo "FAIL: arm64 not Mach-O arm64"; ok=0; }

# 3. -c: object only, still carries the merged Darwin runtime
$PLI --triple arm64-apple-macosx15.0 "$OUT.pli" -c -o "$OUT.arm64.o" \
  || { echo "FAIL: arm64 -c"; ok=0; }

# 4. -shared: dylib (MachO -dylib); FileType: Dylib is the gate. Symbol
#    export/reexport correctness is out of scope for this check.
$PLI --triple arm64-apple-macosx15.0 "$OUT.pli" -shared -o "$OUT.dylib" \
  || { echo "FAIL: arm64 -shared"; ok=0; }
"$READOBJ" --file-headers "$OUT.dylib" | grep -q "FileType: Dylib" \
  || { echo "FAIL: -shared not Dylib"; ok=0; }

# Optional: when run ON macOS, execute the binaries natively.
if [ "$(uname -s)" = "Darwin" ]; then
  [ "$("$OUT.arm64" 2>/dev/null | tr -d '\r')" = "PASS macos_cross 42" ] \
    || { echo "FAIL: arm64 run"; ok=0; }
fi

[ "$ok" = 1 ] && echo "PASS macos_cross"
exit $([ "$ok" = 1 ] && echo 0 || echo 1)
