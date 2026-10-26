#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/windows_math.sh — intrinsic-lowered math on Windows must use
# the UCRT double routines, not MinGW's long-double wrappers.
set -u
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*|Windows_NT*) ;;
  *) echo "SKIP: Windows-only math link check"; exit 0 ;;
esac
PLI=${PLI:-./build/cmake/pli.exe}
OUT=tests/driver/out/windows_math
mkdir -p tests/driver/out

# llvm-nm ships beside the C compiler the suite already passes in.
NM=
if [ -n "${CLANG:-}" ]; then
  for name in llvm-nm llvm-nm.exe; do
    cand="$(dirname "$CLANG")/$name"
    if [ -x "$cand" ]; then NM=$cand; break; fi
  done
fi
if [ -z "$NM" ]; then
  NM=$(command -v llvm-nm 2>/dev/null || true)
fi
[ -n "$NM" ] || { echo "SKIP: llvm-nm not found"; exit 0; }

cat > "$OUT.pli" <<'EOF'
  win_math: procedure options(main);
     declare (s, c, e, l, r, f, ce) float decimal(16);
     s = sin(0.0e0);
     c = cos(0.0e0);
     e = exp(0.0e0);
     l = log(1.0e0);
     r = sqrt(4.0e0);
     f = floor(2.9e0);
     ce = ceil(2.1e0);
     if s = 0.0e0 & c = 1.0e0 & e = 1.0e0 & l = 0.0e0 &
        r = 2.0e0 & f = 2.0e0 & ce = 3.0e0 then
        put skip list('PASS windows_math');
     else
        put skip list('FAIL windows_math');
  end win_math;
EOF

ok=1
fail() { echo "FAIL: $1"; ok=0; }
"$PLI" "$OUT.pli" -O2 -o "$OUT.exe" || fail "math link"
got=$("$OUT.exe" | tr -d '\r') || fail "math run"
[ "$got" = "PASS windows_math" ] || fail "math values"
# The UCRT import must be used, and MinGW's long-double wrapper must not.
"$NM" "$OUT.exe" | grep -q "__imp_sin" || fail "sin does not use UCRT import"
"$NM" "$OUT.exe" | grep -q "__imp_log" || fail "log does not use UCRT import"
"$NM" "$OUT.exe" | grep -q "__imp_floor" || fail "floor does not use UCRT import"
"$NM" "$OUT.exe" | grep -q "__sinl_internal" && fail "MinGW sin wrapper linked"
"$NM" "$OUT.exe" | grep -q "__logl_internal" && fail "MinGW log wrapper linked"

[ "$ok" -eq 1 ] && echo PASS
exit 0
