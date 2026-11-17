#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/bad_pli_prefix.sh — reserved-prefix warning (ADR-393).

set -u
PLI=${PLI:-./build/cmake/pli}
OUT=tests/driver/out/bad_pli_prefix
mkdir -p "$OUT"

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
  *) EXE= ;;
esac

fail() { echo "FAIL bad_pli_prefix: $1"; exit 1; }

cat > "$OUT.pli" <<'EOF'
 pli_fixed_overflow: proc;
   return;
 end pli_fixed_overflow;
EOF

# Compile and capture stderr; the warning must appear.
$PLI -fsyntax-only "$OUT.pli" 2>"$OUT.err" || fail "compilation failed"
grep -q "runtime-reserved symbol" "$OUT.err" || fail "expected reserved-prefix warning not found: $(cat $OUT.err)"

echo "PASS bad_pli_prefix"
exit 0