#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli: a PL/I compiler targeting LLVM
# tests/driver/rt_platform.sh — the split runtime: the portable pli_runtime.bc
# plus a swappable platform TU (--rt-platform). A custom platform file that
# includes the default TU links and runs identically (its marker symbol proves
# the file is what got linked); an incomplete platform is rejected by name;
# a missing platform file is an error.
set -u
# Windows (MSYS/MinGW/Cygwin sh): linked binaries need a .exe suffix, there
# is no libm (the UCRT provides it), and program output uses CRLF. On Unix
# EXE is empty, MATHLIB stays -lm, and stripping CR is a no-op.
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe; MATHLIB= ;;
  *) EXE=; MATHLIB=-lm ;;
esac
PLI=${PLI:-./build/cmake/pli}
CLANG=${CLANG:-clang}
OUT=tests/driver/out/rt_platform
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

# --- seam exercise: ALLOCATE (malloc), DATE (localtime), SYSTEM (popen) ----
cat > "$OUT.pli" <<'EOF'
 rtp_main: procedure options(main);
    declare p pointer;
    declare 1 rec based(p),
           2 a fixed bin(31);
    declare code fixed bin(31);
    declare d char(8);
    allocate rec set(p);
    p -> rec.a = 41;
    d = date();
    code = system('exit 3');
    if p -> rec.a ^= 41 then put skip list('FAIL alloc');
    else if length(d) ^= 8 then put skip list('FAIL date');
    else if code ^= 3 then put skip list('FAIL system');
    else put skip list('PASS rt_platform');
    free rec;
 end rtp_main;
EOF

$PLI "$OUT.pli" -o "$OUT$EXE" || { echo "FAIL: default build"; ok=0; }
check "default run" "$($OUT$EXE)" "PASS rt_platform"

# --- custom platform: the default TU plus a marker proving this file linked -
cat > "$OUT.plat.c" <<'EOF'
#include "rt_plat_default.c"
/* Marker: absent from the default blob, so its presence in the linked
 * binary proves the --rt-platform file is what got linked. */
int pli_plat_test_marker(void) { return 42; }
EOF
$CLANG -emit-llvm -c "$OUT.plat.c" -I runtime -o "$OUT.plat.bc" \
  || { echo "FAIL: custom platform bitcode build"; ok=0; }
$PLI "$OUT.pli" -o "$OUT.custom$EXE" --rt-platform "$OUT.plat.bc" \
  || { echo "FAIL: --rt-platform build"; ok=0; }
check "custom platform run" "$($OUT.custom$EXE)" "PASS rt_platform"

# --- the custom TU's own code runs: the program calls its marker -----------
cat > "$OUT.marker.pli" <<'EOF'
 rtp_marker: procedure options(main);
    declare pli_plat_test_marker entry()
       returns(fixed bin(31))
       options(linkage(system))
       external('pli_plat_test_marker');
    declare m fixed bin(31);
    m = pli_plat_test_marker();
    if m ^= 42 then put skip list('FAIL marker');
    else put skip list('PASS rt_platform marker');
 end rtp_marker;
EOF
$PLI "$OUT.marker.pli" -o "$OUT.marker$EXE" --rt-platform "$OUT.plat.bc" \
  || { echo "FAIL: marker build"; ok=0; }
check "marker run" "$($OUT.marker$EXE)" "PASS rt_platform marker"

# --- archive path: --no-bitcode-runtime also honors --rt-platform -----------
$PLI "$OUT.pli" -o "$OUT.arch$EXE" --no-bitcode-runtime \
  --rt-platform "$OUT.plat.bc" $MATHLIB \
  || { echo "FAIL: archive-path --rt-platform build"; ok=0; }
check "archive-path run" "$($OUT.arch$EXE)" "PASS rt_platform"

# --- incomplete platform: rejected by name, not by a bare link error -------
cat > "$OUT.empty.c" <<'EOF'
#include <stddef.h>
void *pli_plat_malloc(size_t n);
void *pli_plat_malloc(size_t n) { (void)n; return 0; }
EOF
$CLANG -emit-llvm -c "$OUT.empty.c" -o "$OUT.empty.bc" \
  || { echo "FAIL: incomplete platform bitcode build"; ok=0; }
if $PLI "$OUT.pli" -o "$OUT.empty$EXE" --rt-platform "$OUT.empty.bc" \
    2> "$OUT.empty.err"; then
  echo "FAIL: incomplete platform unexpectedly linked"
  ok=0
else
  grep -q "incomplete platform runtime" "$OUT.empty.err" \
    || { echo "FAIL: missing 'incomplete platform runtime' diagnostic"; ok=0; }
  grep -q "pli_plat_free" "$OUT.empty.err" \
    || { echo "FAIL: diagnostic does not name a missing seam symbol"; ok=0; }
fi

# --- missing platform file is a user error -----------------------------------
if $PLI "$OUT.pli" -o "$OUT.missing$EXE" \
    --rt-platform "$OUT.no-such-file.bc" 2> "$OUT.missing.err"; then
  echo "FAIL: missing platform file unexpectedly accepted"
  ok=0
else
  grep -q "no such platform runtime" "$OUT.missing.err" \
    || { echo "FAIL: missing missing-file diagnostic"; ok=0; }
fi

[ "$ok" -eq 1 ] && echo PASS
exit 0
