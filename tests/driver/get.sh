#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM
# tests/driver/get.sh — GET LIST (rules 104-109): list-directed input reads
# numeric and character values from SYSIN (stdin) into variables. The program
# verifies the read values itself and prints PASS.
set -u
PLIC=./build/plic
OUT=./tests/driver/out/get
mkdir -p tests/driver/out

cat > "$OUT.pli" <<'EOF'
 get_test: procedure options(main);
    declare fails fixed bin(31);
    declare a fixed bin(31);
    declare b fixed bin(31);
    declare x float;
    declare s char(8);
    fails = 0;
    get list (a, b, x, s);
    if a ^= 10 then do;
       fails = fails + 1;
       put skip list('FAIL a');
    end;
    if b ^= 20 then do;
       fails = fails + 1;
       put skip list('FAIL b');
    end;
    if x ^= 3.5 then do;
       fails = fails + 1;
       put skip list('FAIL x');
    end;
    if s ^= 'hello' then do;
       fails = fails + 1;
       put skip list('FAIL s');
    end;
    if fails = 0 then put skip list('PASS get');
    else put skip list('FAIL get: fails =', fails);
 end get_test;
EOF

$PLIC "$OUT.pli" -o "$OUT" || { echo "FAIL get: compile"; exit 1; }
printf '10 20 3.5 hello\n' | "$OUT"
exit 0
