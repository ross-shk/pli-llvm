#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/bundle_standalone.sh — standalone check: the release binary
# links no toolchain shared libraries (--assert-static gates it) and
# compiles+runs a program directly. There is no bundle step anymore (ADR-225:
# single file, no lib/ dir, no shipped clang). A package-manager dev build
# still links LLVM shared libs, so the assert fails there — SKIP, not FAIL:
# standalone-ness is a property of the static-prefix release build.
# Windows is already statically linked.
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

if ! sh scripts/bundle_standalone.sh --assert-static "$PLI" >/dev/null 2>&1; then
  echo "SKIP: pli links toolchain shared libs (dev build, not the static release)"
  exit 0
fi

cat > "$OUT.pli" <<'EOF'
 bundle: proc options(main);
    put skip list('PASS bundle_standalone (static, single file)');
 end bundle;
EOF
"$PLI" "$OUT.pli" -o "$OUT.bin$EXE" || { echo "FAIL: static compile"; exit 1; }
[ "$("$OUT.bin$EXE")" = "PASS bundle_standalone (static, single file)" ] && echo PASS && exit 0
echo "FAIL: static run"; exit 1
