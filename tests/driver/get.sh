#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/get.sh — GET LIST (rules 104-109): list-directed input reads
# numeric and character values from SYSIN (stdin) into variables. The program
# verifies the read values itself and prints PASS.
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=./tests/driver/out/get
mkdir -p tests/driver/out

cat > "$OUT.pli" <<'EOF'
 get_test: procedure options(main);
    declare fails fixed bin(31);
    declare a fixed bin(31);
    declare b fixed bin(31);
    declare x float;
    declare s char(8);
    declare v char(8) varying;
    declare v2 char(4) varying;
    fails = 0;
    get list (a, b, x, s, v, v2);
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
    if length(s) ^= 8 then do;
       fails = fails + 1;
       put skip list('FAIL s length', length(s));
    end;
    if length(v) ^= 5 then do;
       fails = fails + 1;
       put skip list('FAIL v length', length(v));
    end;
    if v ^= 'Alice' then do;
       fails = fails + 1;
       put skip list('FAIL v', v);
    end;
    if 'Hello, ' || v || '!' ^= 'Hello, Alice!' then do;
       fails = fails + 1;
       put skip list('FAIL greeting');
    end;
    if length('Hello, ' || v || '!') ^= 13 then do;
       fails = fails + 1;
       put skip list('FAIL greeting length');
    end;
    if length(v2) ^= 4 then do;
       fails = fails + 1;
       put skip list('FAIL v2 length', length(v2));
    end;
    if v2 ^= 'abcd' then do;
       fails = fails + 1;
       put skip list('FAIL v2', v2);
    end;
    if fails = 0 then put skip list('PASS get');
    else put skip list('FAIL get: fails =', fails);
 end get_test;
EOF

$PLI "$OUT.pli" -o "$OUT" || { echo "FAIL get: compile"; exit 1; }
printf '10 20 3.5 hello Alice abcdef\n' | "$OUT"
exit 0
