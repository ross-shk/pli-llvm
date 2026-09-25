#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM
# tests/driver/float_fixed_overflow.sh — QR1.2 FLOAT -> FIXED conversion
# boundaries: an out-of-range float traps with an ERROR (FPToSI is UB there;
# hard error until a CONVERSION/SIZE condition can route it); fitting edges,
# including exact INT32_MIN and scaled decimals, run clean.
set -u
PLIC=./build/plic
OUT=./tests/driver/out/float_fixed_overflow
mkdir -p tests/driver/out

gen() {
cat > "$OUT.$1.pli" <<EOF
 float_fixed_overflow_$1: procedure options(main);
 $2
 $3
 end float_fixed_overflow_$1;
EOF
}

# Each overflow case must abort (rc != 0) with the overflow message.
expect_abort() {
  $PLIC "$OUT.$1.pli" -o "$OUT.$1" 2>/dev/null || { echo "FAIL float-fixed-overflow: $1 compile"; exit 1; }
  out=$("$OUT.$1" 2>&1); rc=$?
  case "$out" in
    *overflow*) ;;
    *) echo "FAIL float-fixed-overflow: $1 wrong message"; exit 1;;
  esac
  if [ "$rc" -eq 0 ]; then echo "FAIL float-fixed-overflow: $1 exited 0"; exit 1; fi
  echo "PASS float-fixed-overflow-$1";
}

# 1e20 needs 21 digits: past both i32 range and (5,2) digits.
gen bin32 "declare f float decimal(6); declare i fixed bin(31);" \
  "f = 1.0e20; i = f; put skip list(i);"
# 1e19 pasts i64 range too.
gen bin64 "declare f float decimal(6); declare i fixed bin(63);" \
  "f = 1.0e19; i = f; put skip list(i);"
# 1e10 scaled by 10^2 pasts (5,2) digits while fitting i32.
gen dec "declare f float decimal(6); declare d fixed decimal(5,2);" \
  "f = 1.0e10; d = f; put skip list(d);"
expect_abort bin32
expect_abort bin64
expect_abort dec

# In-range edges run and print.
cat > "$OUT.ok.pli" <<'EOF'
 float_fixed_overflow_ok: procedure options(main);
    declare f float decimal(6);
    declare i fixed binary(31);
    declare d fixed decimal(5,2);
    f = 2147483647.0;
    i = f;
    put skip list(i);
    f = -2147483648.0;
    i = f;
    put skip list(i);
    f = 999.99;
    d = f;
    put skip list(d);
    f = -999.99;
    d = f;
    put skip list(d);
 end float_fixed_overflow_ok;
EOF
$PLIC "$OUT.ok.pli" -o "$OUT.ok" || { echo "FAIL float-fixed-overflow: ok compile"; exit 1; }
if [ "$("$OUT.ok")" = "2147483647
-2147483648
999.99
-999.99" ]; then
  echo "PASS float-fixed-overflow-ok";
else
  echo "FAIL float-fixed-overflow: ok output"; exit 1;
fi
