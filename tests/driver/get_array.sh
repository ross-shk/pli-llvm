#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM
# tests/driver/get_array.sh — aggregate GET LIST items (rules (104)-(110)):
# whole arrays and cross-sections read element-wise in row-major order.
# The program verifies the read values itself and prints PASS.
set -u
PLIC=./build/plic
OUT=./tests/driver/out/get_array
mkdir -p tests/driver/out

cat > "$OUT.pli" <<'EOF'
 get_array: procedure options(main);
    declare fails fixed bin(31);
    declare a(3) fixed bin(31);
    declare m(2, 2) fixed bin(31);
    fails = 0;
    m(2, 1) = 7;
    m(2, 2) = 8;
    get list (a, m(1, *));
    if a(1) ^= 10 then fails = fails + 1;
    if a(2) ^= 20 then fails = fails + 1;
    if a(3) ^= 30 then fails = fails + 1;
    if m(1, 1) ^= 1 then fails = fails + 1;
    if m(1, 2) ^= 2 then fails = fails + 1;
    if m(2, 1) ^= 7 then fails = fails + 1;
    if m(2, 2) ^= 8 then fails = fails + 1;
    if fails = 0 then put skip list('PASS get array');
    else put skip list('FAIL get array: fails =', fails);
 end get_array;
EOF

$PLIC "$OUT.pli" -o "$OUT" || { echo "FAIL get array: compile"; exit 1; }
printf '10 20 30 1 2\n' | "$OUT"
exit 0
