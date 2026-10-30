#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/flto.sh — explicit opt-in LTO (OPTIMIZATION.md §10, P3),
# fully in-process (ADR-225): --flto=thin/full emit LTO bitcode objects that
# in-process lld optimizes at link. No clang anywhere: -v output must never
# mention it.
#
# The link needs ONE LLVM instance in the process: a shared lld drags in the
# shared libLLVM, and the compiler's own symbols must come from that same
# instance (ADR-268). The build picks the one that matches the lld it links,
# so the link+run below must work on every supported configuration.
set -u
# Windows (MSYS/MinGW/Cygwin sh): linked binaries need a .exe suffix, there
# is no libm (the UCRT provides it), and program output uses CRLF. On Unix
# EXE is empty, MATHLIB stays -lm, and stripping CR is a no-op.
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe; MATHLIB= ;;
  *) EXE=; MATHLIB=-lm ;;
esac
PLI=${PLI:-./build/cmake/pli}
OUT=tests/driver/out/flto
mkdir -p tests/driver/out

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
no_clang() {
  # The -v transcript must never INVOKE clang. Library paths (lib/clang/...)
  # and data files (clang_rt.*, libclang_rt.*.a lld inputs) are files, not
  # processes, so drop those lines first; any clang left on a '+' command
  # line is a subprocess.
  stripped=$(printf '%s\n' "$2" | grep -v 'lib/clang/' | grep -v 'clang_rt')
  if printf '%s\n' "$stripped" | grep -q '^\+.*clang'; then
    echo "FAIL: $1 invokes clang (must be fully in-process)"
    ok=0
  fi
}
is_bitcode() {
  # An LTO object is LLVM bitcode, not a native object.
  if command -v llvm-bcanalyzer >/dev/null 2>&1; then
    llvm-bcanalyzer --dump "$1" >/dev/null 2>&1 \
      || { echo "FAIL: $2 is not LLVM bitcode"; ok=0; }
  elif command -v file >/dev/null 2>&1; then
    file "$1" | grep -qi "bitcode" \
      || { echo "FAIL: $2 is not LLVM bitcode"; ok=0; }
  else
    echo "SKIP: no llvm-bcanalyzer/file to inspect $2"
  fi
}

cat > "$OUT.pli" <<'EOF'
 flto_main: procedure options(main);
    put skip list('PASS flto');
 end flto_main;
EOF

# --- ThinLTO object emission (runs everywhere) -------------------------
cmd=$($PLI "$OUT.pli" -c -o "$OUT.thin.o" --flto=thin -v 2>&1) \
  || { echo "FAIL: thin-lto compile"; ok=0; }
no_clang "thin-lto -v" "$cmd"
is_bitcode "$OUT.thin.o" "thin-lto object"

# --- full-LTO object emission (runs everywhere) -------------------------
cmd=$($PLI "$OUT.pli" -c -o "$OUT.full.o" --flto=full -v 2>&1) \
  || { echo "FAIL: full-lto compile"; ok=0; }
no_clang "full-lto -v" "$cmd"
is_bitcode "$OUT.full.o" "full-lto object"

# --- full link+run ---------------------------------------------------------
cmd=$($PLI "$OUT.pli" -o "$OUT.thin$EXE" --flto=thin -v 2>&1) \
  || { echo "FAIL: thin-lto compile+link"; ok=0; }
no_clang "thin-lto link -v" "$cmd"
check "thin run" "$($OUT.thin$EXE)" "PASS flto"

$PLI "$OUT.pli" -o "$OUT.full$EXE" --flto=full \
  || { echo "FAIL: full-lto compile+link"; ok=0; }
check "full run" "$($OUT.full$EXE)" "PASS flto"

cat > "$OUT.lib.pli" <<'EOF'
 flto_lib: procedure(x) returns(fixed bin(31));
    declare x fixed bin(31);
    return(x * 2);
 end flto_lib;
EOF
cat > "$OUT.main.pli" <<'EOF'
 flto_main2: procedure options(main);
    declare flto_lib entry(fixed bin(31)) returns(fixed bin(31)) external;
    put skip list('PASS flto', flto_lib(21));
 end flto_main2;
EOF
$PLI "$OUT.lib.pli" "$OUT.main.pli" -o "$OUT.multi$EXE" --flto=thin \
  || { echo "FAIL: multi-unit thin-lto"; ok=0; }
check "multi thin run" "$($OUT.multi$EXE)" "PASS flto 42"

[ "$ok" -eq 1 ] && echo PASS
exit 0
