#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/windows_subsystem.sh — the PE subsystem is console by default,
# so exec tests keep their stdout; --subsystem=windows selects a GUI binary.
set -u
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*|Windows_NT*) ;;
  *) echo "SKIP: PE subsystem is Windows-only"; exit 0 ;;
esac
PLI=${PLI:-./build/cmake/pli.exe}
OUT=tests/driver/out/windows_subsystem
mkdir -p tests/driver/out

# llvm-readobj ships beside the C compiler the suite already passes in.
READOBJ=
if [ -n "${CLANG:-}" ]; then
  for name in llvm-readobj llvm-readobj.exe; do
    cand="$(dirname "$CLANG")/$name"
    if [ -x "$cand" ]; then READOBJ=$cand; break; fi
  done
fi
if [ -z "$READOBJ" ]; then
  READOBJ=$(command -v llvm-readobj 2>/dev/null || true)
fi
[ -n "$READOBJ" ] || { echo "SKIP: llvm-readobj not found"; exit 0; }

cat > "$OUT.pli" <<'EOF'
 subsystem_test: procedure options(main);
    put skip list('PASS windows_subsystem');
 end subsystem_test;
EOF

ok=1
fail() { echo "FAIL: $1"; ok=0; }
subsystem_of() {
  "$READOBJ" --file-headers "$1" 2>/dev/null |
    sed -n 's/.*Subsystem: IMAGE_SUBSYSTEM_\([A-Z_]*\).*/\1/p'
}

# Console is the default: PUT output must reach the inherited stdout.
"$PLI" "$OUT.pli" -o "$OUT.console.exe" || fail "default link"
[ "$(subsystem_of "$OUT.console.exe")" = "WINDOWS_CUI" ] ||
  fail "default subsystem is not console"
got=$("$OUT.console.exe" | tr -d '\r') || fail "default binary run"
[ "$got" = "PASS windows_subsystem" ] || fail "default binary output"

# Both spellings reach the PE header.
"$PLI" "$OUT.pli" -o "$OUT.console2.exe" --subsystem=console || fail "explicit console link"
[ "$(subsystem_of "$OUT.console2.exe")" = "WINDOWS_CUI" ] ||
  fail "--subsystem=console not applied"
"$PLI" "$OUT.pli" -o "$OUT.gui.exe" --subsystem=windows || fail "GUI link"
[ "$(subsystem_of "$OUT.gui.exe")" = "WINDOWS_GUI" ] ||
  fail "--subsystem=windows not applied"
"$PLI" "$OUT.pli" -o "$OUT.gui2.exe" --subsystem windows || fail "separate --subsystem link"
[ "$(subsystem_of "$OUT.gui2.exe")" = "WINDOWS_GUI" ] ||
  fail "separate --subsystem not applied"

# An unknown subsystem is a typo, not a silent fallback.
"$PLI" "$OUT.pli" -o "$OUT.bad.exe" --subsystem=efi >/dev/null 2>&1 &&
  fail "unknown subsystem accepted"

[ "$ok" -eq 1 ] && echo PASS
exit 0
