#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM
# tests/driver/bitcode_runtime.sh — OPTIMIZATION.md §10 (P3): the bitcode
# runtime. IRGen merges the pli_* definitions from runtime.bc into the MAIN
# unit (full merge, ADR-179 — library units reference symbols the MAIN unit
# never calls, so a LinkOnlyNeeded pull would drag the archive back in and
# duplicate them); the archive stays on the link line, never pulled once the
# symbols are defined; --no-bitcode-runtime compiles every unit against the
# sectioned archive; a runtime.bc not built by this LLVM is diagnosed at load
# time; a missing explicitly named runtime.bc is an error.
set -u
# Windows (MSYS/MinGW/Cygwin sh): linked binaries need a .exe suffix, there
# is no libm (the UCRT provides it), and program output uses CRLF. On Unix
# EXE is empty, MATHLIB stays -lm, and stripping CR is a no-op.
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe; MATHLIB= ;;
  *) EXE=; MATHLIB=-lm ;;
esac
PLIC=${PLIC:-./build/plic}
OUT=tests/driver/out/bitcode_runtime
OUTDIR=tests/driver/out
mkdir -p "$OUTDIR"

ok=1
check() {
  # Strip CR: Windows programs emit CRLF (a no-op on Unix).
  set -- "$1" "$(printf '%s' "$2" | tr -d '\r')" "$3"
  if [ "$2" != "$3" ]; then
    echo "FAIL: $1"
    echo "  expected: $3"
    echo "  got:      $2"
    ok=0
  fi
}

cat > "$OUT.pli" <<'EOF'
 br_main: procedure options(main);
    put skip list('PASS bitcode_runtime');
 end br_main;
EOF

# --- default: the MAIN unit embeds the runtime, so --keep-ll shows the
#     pli_* bodies (external linkage) instead of bare declarations ------
$PLIC "$OUT.pli" -o "$OUT.bc$EXE" --keep-ll || { echo "FAIL: default build"; ok=0; }
grep -q "define.*@pli_put_list_char" "$OUT.ll" \
  || { echo "FAIL: kept .ll lacks an embedded runtime body"; ok=0; }
grep -q "declare.*@pli_put_list_char" "$OUT.ll" \
  && { echo "FAIL: runtime still a bare declaration"; ok=0; }
check "default run" "$($OUT.bc$EXE)" "PASS bitcode_runtime"

# --- --no-bitcode-runtime: back to the sectioned archive, so the module
#     carries only declarations -----------------------------------------
$PLIC "$OUT.pli" -o "$OUT.nobc$EXE" --no-bitcode-runtime --keep-ll \
  || { echo "FAIL: --no-bitcode-runtime build"; ok=0; }
grep -q "declare.*@pli_put_list_char" "$OUT.ll" \
  || { echo "FAIL: archive mode lacks a pli_* declaration"; ok=0; }
grep -q "define.*@pli_put_list_char" "$OUT.ll" \
  && { echo "FAIL: archive mode unexpectedly embeds the runtime"; ok=0; }
check "archive run" "$($OUT.nobc$EXE)" "PASS bitcode_runtime"

# --- multi-unit: only the MAIN object embeds the runtime; the library unit's
#     external pli_* references resolve against that one shared copy (runtime
#     globals stay shared) with no duplicate symbols ---------------------
cat > "$OUT.lib.pli" <<'EOF'
 br_lib: procedure;
    put skip list('lib unit');
 end br_lib;
EOF
cat > "$OUT.main.pli" <<'EOF'
 br_main: procedure options(main);
    declare br_lib entry external;
    call br_lib;
    put skip list('PASS bitcode_runtime');
 end br_main;
EOF
$PLIC "$OUT.lib.pli" "$OUT.main.pli" -o "$OUT.multi$EXE" \
  || { echo "FAIL: multi-unit bitcode link"; ok=0; }
check "multi-unit run" "$($OUT.multi$EXE | tr -d '\r\n')" "lib unitPASS bitcode_runtime"

# --- a runtime.bc that this plic did not build is rejected at load -----
cat > "$OUT.fake.ll" <<'EOF'
define void @not_plic_runtime() {
  ret void
}
EOF
if $PLIC "$OUT.pli" -o "$OUT.stamp" --runtime-bc "$OUT.fake.ll" \
     >/dev/null 2>"$OUT.stamp.err"; then
  echo "FAIL: unstamped runtime.bc not diagnosed"
  ok=0
else
  grep -q "LLVM" "$OUT.stamp.err" || { echo "FAIL: version-stamp diagnostic missing"; ok=0; }
fi

# --- an explicitly named runtime.bc that does not exist is an error ----
if $PLIC "$OUT.pli" -o "$OUT.missing" --runtime-bc "$OUTDIR/nope.bc" \
     >/dev/null 2>"$OUT.missing.err"; then
  echo "FAIL: missing --runtime-bc not diagnosed"
  ok=0
fi

[ "$ok" -eq 1 ] && echo PASS
exit 0
