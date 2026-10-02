#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM
# tests/driver/pgo.sh — explicit opt-in PGO (OPTIMIZATION.md §10, P3):
# --fprofile-generate instruments the build, the instrumented binary writes
# a profile, --fprofile-use= rebuilds with it; a stale profile for different
# code is tolerated (profile-mismatch case) without breaking the build.
set -u
PLIC=./build/plic
OUT=tests/driver/out/pgo
mkdir -p "$OUT"

ok=1
check() {
  if [ "$2" != "$3" ]; then
    echo "FAIL: $1"
    echo "  expected: $3"
    echo "  got:      $2"
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

# --- phase 1: instrument, run to collect the profile -------------------
cmd=$($PLIC "$OUT.pli" -o "$OUT.gen" --fprofile-generate -v 2>&1) \
  || { echo "FAIL: pgo generate build"; ok=0; }
printf '%s\n' "$cmd" | grep -q -- '-fprofile-generate' \
  || { echo "FAIL: -fprofile-generate not passed through"; ok=0; }
# Run once, inside the per-test directory: the instrumented binary drops
# default_<id>_0.profraw in its working directory, which must not be the
# shared repo root under concurrent suites.
phase1_out=$(cd "$OUT" && ../pgo.gen)
check "phase1 run" "$phase1_out" "PASS pgo 500500"
PROFRAW=$(ls "$OUT"/default_*.profraw 2>/dev/null | head -1)
if [ -z "$PROFRAW" ]; then
  echo "FAIL: no profile written by the instrumented run"
  ok=0
fi

# llvm-profdata lives next to the clang plic bakes in (llvm-profdata is not
# assumed on PATH): recover its directory from a -v compile command.
CLDIR=$(printf '%s\n' "$cmd" | grep -o "'[^']*bin/clang'" | head -1 | tr -d "'")
CLDIR=$(dirname "$CLDIR")
if [ ! -x "$CLDIR/llvm-profdata" ]; then
  echo "FAIL: llvm-profdata not found next to $CLDIR/clang"
  ok=0
fi

"$CLDIR/llvm-profdata" merge --failure-mode=warn -output="$OUT.profdata" "$PROFRAW" \
  || { echo "FAIL: llvm-profdata merge"; ok=0; }

# --- phase 2: rebuild using the profile --------------------------------
$PLIC "$OUT.pli" -o "$OUT.use" --fprofile-use="$OUT.profdata" \
  || { echo "FAIL: pgo use build"; ok=0; }
check "phase2 run" "$($OUT.use)" "PASS pgo 500500"

# --- profile mismatch: a different program built with the same profile
#     still compiles and runs (clang tolerates stale counters) ----------
cat > "$OUT.other.pli" <<'EOF'
 pgo_other: procedure options(main);
    put skip list('PASS pgo other');
 end pgo_other;
EOF
$PLIC "$OUT.other.pli" -o "$OUT.mismatch" --fprofile-use="$OUT.profdata" \
  || { echo "FAIL: mismatch build"; ok=0; }
check "mismatch run" "$($OUT.mismatch)" "PASS pgo other"

[ "$ok" -eq 1 ] && echo PASS
exit 0
