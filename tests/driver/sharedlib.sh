#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/sharedlib.sh — -shared emits a shared library (.so/.dylib/.dll)
# through the in-process lld link step, with no CRT/entry/subsystem and the
# format-appropriate shared flag (-shared / -dynamiclib / /DLL).
set -u
case "$(uname -s)" in
  Darwin*) SHLIB=.dylib ;;
  MINGW*|MSYS*|CYGWIN*) SHLIB=.dll ;;
  *) SHLIB=.so ;;
esac
PLI=${PLI:-./build/cmake/pli}
# Make PLI absolute so it still resolves after we cd into a scratch dir.
# Preserve already-absolute paths (Unix /... or Windows C:\...).
case "$PLI" in
  /*) ;;
  [A-Za-z]:*) ;;
  *) PLI="$PWD/$PLI" ;;
esac
OUT=tests/driver/out/sharedlib
# Absolute so the `cd` subshell below can still locate the fixture.
FIX="$PWD/$OUT.pli"
mkdir -p "$OUT"

ok=1
fail() { echo "FAIL: $1"; ok=0; }

# PL/I shared-lib module: no OPTIONS(MAIN), EXTERNAL-only surface.
cat > "$FIX" <<'EOF'
 sharedlib: procedure;
   put skip list('sharedlib module');
 end sharedlib;
EOF

# --- default output name: a.so / a.dll / a.dylib ---------------------------
mkdir -p "$OUT/dflt"
( cd "$OUT/dflt" && $PLI "$FIX" -shared >/dev/null 2>&1 ) \
  || fail "-shared default build failed"
[ -f "$OUT/dflt/a$SHLIB" ] || fail "default shared lib a$SHLIB not produced"

# --- explicit -o name ------------------------------------------------------
$PLI "$FIX" -shared -o "$OUT/libtest$SHLIB" >/dev/null 2>&1 \
  || fail "-shared -o build failed"
[ -f "$OUT/libtest$SHLIB" ] || fail "explicit -o shared lib not produced"

# --- -v shows the format shared flag ---------------------------------------
cmd=$($PLI "$FIX" -shared -o "$OUT/libv$SHLIB" -v 2>&1)
case "$SHLIB" in
  .dylib) want=-dynamiclib ;;
  .dll)   want=/DLL ;;
  .so)    want=-shared ;;
esac
printf '%s\n' "$cmd" | grep -q -- "$want" || fail "$want not in link command"

[ "$ok" -eq 1 ] && echo PASS
exit 0
