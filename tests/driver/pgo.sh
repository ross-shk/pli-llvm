#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/pgo.sh — explicit opt-in PGO (OPTIMIZATION.md §10, P3),
# fully in-process (ADR-225): --fprofile-generate instruments via PassBuilder
# and links the compiler-rt profile runtime with in-process lld;
# --fprofile-use= consumes a raw .profraw OR an indexed .profdata directly
# (no llvm-profdata merge step). No clang anywhere: -v output must never
# mention it. A stale profile for different code is a hard build error
# (in-process use validates function names), not silently tolerated.
set -u
# Windows (MSYS/MinGW/Cygwin sh): linked binaries need a .exe suffix, there
# is no libm (the UCRT provides it), and program output uses CRLF. On Unix
# EXE is empty, MATHLIB stays -lm, and stripping CR is a no-op.
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe; MATHLIB= ;;
  *) EXE=; MATHLIB=-lm ;;
esac
PLI=${PLI:-./build/cmake/pli}
OUT=tests/driver/out/pgo
mkdir -p "$OUT"

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

cat > "$OUT.pli" <<'EOF'
 pgo_main: procedure options(main);
    declare i fixed bin(31);
    declare s fixed bin(31);
    s = 0;
    do i = 1 to 1000;
       s = s + i;
    end;
    put skip list('PASS pgo', s);
 end pgo_main;
EOF

# Some builds omit the compiler-rt profile runtime: probe with the exact
# generate command and skip (not fail) when it cannot link.
if ! $PLI "$OUT.pli" -o "$OUT.probe$EXE" --fprofile-generate >/dev/null 2>&1; then
  echo "SKIP: profile runtime (clang_rt.profile) missing from this toolchain"
  exit 0
fi
rm -f "$OUT.probe$EXE"

# --- phase 1: instrument, run to collect the profile -------------------
cmd=$($PLI "$OUT.pli" -o "$OUT.gen$EXE" --fprofile-generate -v 2>&1) \
  || { echo "FAIL: pgo generate build"; ok=0; }
no_clang "pgo generate -v" "$cmd"
# Deterministic profile path (no default_*.profraw glob, no stale leftovers).
rm -f "$OUT.profraw"
phase1_out=$(LLVM_PROFILE_FILE="$OUT.profraw" "$OUT.gen$EXE")
check "phase1 run" "$phase1_out" "PASS pgo 500500"
[ -s "$OUT.profraw" ] || { echo "FAIL: no profile written by the instrumented run"; ok=0; }

# --- phase 2: rebuild using the RAW profile (no merge step) ------------
cmd=$($PLI "$OUT.pli" -o "$OUT.use$EXE" --fprofile-use="$OUT.profraw" -v 2>&1) \
  || { echo "FAIL: pgo use build"; ok=0; }
no_clang "pgo use -v" "$cmd"
check "phase2 run" "$($OUT.use$EXE)" "PASS pgo 500500"

# --- stale profile: a different program built with the same profile -----
#     must fail loudly (function-name validation), not silently miscompile.
cat > "$OUT.other.pli" <<'EOF'
 pgo_other: procedure options(main);
    put skip list('PASS pgo other');
 end pgo_other;
EOF
if $PLI "$OUT.other.pli" -o "$OUT.mismatch$EXE" --fprofile-use="$OUT.profraw" \
    >/dev/null 2>&1; then
  echo "FAIL: stale profile should be rejected"
  ok=0
fi

[ "$ok" -eq 1 ] && echo PASS
exit 0
