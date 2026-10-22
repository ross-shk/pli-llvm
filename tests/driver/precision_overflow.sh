#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/precision_overflow.sh — ADR-280: SIZE enforces the declared
# precision of FIXED BINARY(p), not the storage width. A value outside
# [-2^p, 2^p-1] aborts when unhandled and resumes wrapped when an ON SIZE
# unit is established; --no-size-checks still elides every trap.
set -u
# Windows (MSYS/MinGW/Cygwin sh): linked binaries need a .exe suffix and
# program output uses CRLF. On Unix EXE is empty and stripping CR is a no-op.
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
  *) EXE= ;;
esac
PLI=${PLI:-./build/cmake/pli}
OUT=./tests/driver/out/precision_overflow
mkdir -p tests/driver/out

# expect_abort NAME SOURCE — the program must compile and abort with overflow.
expect_abort() {
  $PLI --size-checks "$OUT.$1.pli" -o "$OUT.$1$EXE" 2>/dev/null || { echo "FAIL precision-overflow: $1 compile"; exit 1; }
  out=$("$OUT.$1$EXE" 2>&1); rc=$?
  case "$out" in
    *overflow*) ;;
    *) echo "FAIL precision-overflow: $1 wrong message: $out"; exit 1;;
  esac
  if [ "$rc" -eq 0 ]; then echo "FAIL precision-overflow: $1 exited 0"; exit 1; fi
  echo "PASS precision-overflow-$1";
}

# A literal 40000 fits the i32 slot but not FIXED BIN(15)'s [-32768, 32767].
cat > "$OUT.bin15.pli" <<'EOF'
 p_bin15: procedure options(main);
    declare x fixed bin(15);
    x = 40000;
    put skip list(x);
 end p_bin15;
EOF
expect_abort bin15

# A literal 200 fits the i32 slot but not FIXED BIN(7)'s [-128, 127].
cat > "$OUT.bin7.pli" <<'EOF'
 p_bin7: procedure options(main);
    declare x fixed bin(7);
    x = 200;
    put skip list(x);
 end p_bin7;
EOF
expect_abort bin7

# The same check runs after a FLOAT conversion and after a widening binary
# source: both reach the target value above the declared range.
cat > "$OUT.bin7f.pli" <<'EOF'
 p_bin7f: procedure options(main);
    declare x fixed bin(7);
    x = 200.0;
    put skip list(x);
 end p_bin7f;
EOF
expect_abort bin7f

cat > "$OUT.bin7w.pli" <<'EOF'
 p_bin7w: procedure options(main);
    declare w fixed bin(63);
    declare x fixed bin(7);
    w = 200;
    x = w;
    put skip list(x);
 end p_bin7w;
EOF
expect_abort bin7w

# Established SIZE handler: each trap resumes with the value wrapped inside
# the declared precision. FIXED BIN(31)'s range is the full i32 slot, so
# 2147483647 is in range and only the arithmetic overflow trips.
cat > "$OUT.resume.pli" <<'EOF'
 p_resume: procedure options(main);
    declare a fixed bin(15);
    declare x fixed bin(7);
    declare f fixed bin(7);
    declare w fixed bin(63);
    declare h fixed bin(7);
    declare b fixed bin(31);
    on size put skip list('caught');
    a = 40000;
    put skip list(a);
    x = 200;
    put skip list(x);
    f = 200.0;
    put skip list(f);
    w = 200;
    h = w;
    put skip list(h);
    b = 2147483647;
    put skip list(b);
 end p_resume;
EOF
$PLI --size-checks "$OUT.resume.pli" -o "$OUT.resume$EXE" || { echo "FAIL precision-overflow: resume compile"; exit 1; }
got=$("$OUT.resume$EXE" | tr -d '\r')
want="caught
-25536
caught
-56
caught
-56
caught
-56
2147483647"
if [ "$got" = "$want" ]; then
  echo "PASS precision-overflow-resume"
else
  echo "FAIL precision-overflow: resume output"
  echo "--- got ---"; echo "$got"
  echo "--- want ---"; echo "$want"
  exit 1
fi

# --no-size-checks elides the precision trap: the wide-slot value stands.
$PLI "$OUT.resume.pli" --no-size-checks -o "$OUT.nosize$EXE" || { echo "FAIL precision-overflow: nosize compile"; exit 1; }
got=$("$OUT.nosize$EXE" | tr -d '\r')
want="40000
200
200
200
2147483647"
if [ "$got" = "$want" ]; then
  echo "PASS precision-overflow-nosize"
else
  echo "FAIL precision-overflow: nosize output"
  echo "--- got ---"; echo "$got"
  exit 1
fi

# A (NOSIZE) prefix elides the same trap for that statement.
cat > "$OUT.prefix.pli" <<'EOF'
 p_prefix: procedure options(main);
    declare x fixed bin(15);
    (nosize): x = 40000;
    put skip list(x);
 end p_prefix;
EOF
$PLI --size-checks "$OUT.prefix.pli" -o "$OUT.prefix$EXE" || { echo "FAIL precision-overflow: prefix compile"; exit 1; }
got=$("$OUT.prefix$EXE" | tr -d '\r')
if [ "$got" = "40000" ]; then
  echo "PASS precision-overflow-prefix"
else
  echo "FAIL precision-overflow: (NOSIZE) prefix output: $got"
  exit 1
fi

exit 0
