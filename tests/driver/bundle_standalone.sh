#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/bundle_standalone.sh — RELEASE.md: the release tarball bundles
# the LLVM closure (lld/libLLVM/libz3/libzstd) so the binary runs standalone
# where LLVM isn't installed; brew/apt ship those shared-only, so the script
# relocates them next to the binary and rewrites rpaths. Only meaningful on
# platforms where the binary links LLVM dynamically (Unix); Windows is already
# statically linked. Needs the tooling (install_name_tool/patchelf/codesign).
set -u
EXE=.exe
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*|Windows_NT*) EXE=.exe ;;
  *) EXE= ;;
esac
PLI=${PLI:-./build/cmake/pli}
OUT=tests/driver/out/bundle_standalone
OUTDIR=tests/driver/out
mkdir -p "$OUTDIR"
BUNDLE=$OUTDIR/bundle

# Linux needs patchelf; macOS needs install_name_tool. Skip when missing.
if [ "$(uname -s)" = "Linux" ] && ! command -v patchelf >/dev/null 2>&1; then
  echo "SKIP: patchelf not installed"; exit 0
fi
if [ "$(uname -s)" = "Darwin" ] && ! command -v install_name_tool >/dev/null 2>&1; then
  echo "SKIP: install_name_tool not installed"; exit 0
fi

rm -rf "$BUNDLE"
sh scripts/bundle_standalone.sh "$PLI" "$BUNDLE" >/dev/null 2>&1 || { echo "FAIL: bundle_standalone.sh"; exit 1; }

ok=1
check() {
  set -- "$1" "$(printf '%s' "$2" | tr -d '\r')" "$3"
  if [ "$2" != "$3" ]; then
    echo "FAIL: $1"; echo "  expected: $3"; echo "  got:      $2"; ok=0
  fi
}

cat > "$OUT.pli" <<'EOF'
 bundle: proc options(main);
    put skip list('PASS bundle_standalone');
 end bundle;
EOF
"$BUNDLE/pli" "$OUT.pli" -o "$OUT.bin$EXE" || { echo "FAIL: bundled compile"; ok=0; }
check "bundled run" "$($OUT.bin$EXE)" "PASS bundle_standalone"

# The bundled binary must not resolve LLVM from the package-manager dirs.
if [ "$(uname -s)" = "Darwin" ]; then
  if otool -L "$BUNDLE/pli" | grep -E "/opt/homebrew|/opt/local|/usr/local" >/dev/null; then
    echo "FAIL: bundled binary still links a Homebrew path"; ok=0
  fi
elif [ "$(uname -s)" = "Linux" ]; then
  if ldd "$BUNDLE/pli" | grep -E "/usr/lib/llvm|/opt/|/usr/local/(llvm|lld)" >/dev/null; then
    echo "FAIL: bundled binary still links a toolchain path"; ok=0
  fi
fi

[ "$ok" -eq 1 ] && echo PASS
exit 0
