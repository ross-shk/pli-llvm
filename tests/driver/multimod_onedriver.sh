#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM
# tests/driver/multimod_onedriver.sh — multi-file driver (rules (2),(38),(42)):
# a single plic invocation compiles a MAIN unit and a library unit and links
# them, replacing the manual -c + clang link of multimod.sh.
set -u
PLIC=./build/plic
OUT=tests/driver/out/multimod_onedriver
mkdir -p tests/driver/out

cat > "$OUT.lib.pli" <<'EOF'
 doubleit: procedure(x) returns(fixed bin(31));
    declare x fixed bin(31);
    return(x * 2);
 end doubleit;
 bump: procedure(x);
    declare x fixed bin(31);
    x = x + 1;
 end bump;
EOF
cat > "$OUT.main.pli" <<'EOF'
 multimain: procedure options(main);
    declare doubleit entry(fixed bin(31))
       returns(fixed bin(31)) external;
    declare bump entry(fixed bin(31)) external;
    declare r fixed bin(31);
    declare s fixed bin(31);
    declare fails fixed bin(31);
    fails = 0;
    r = doubleit(21);
    if r ^= 42 then do;
       fails = fails + 1;
       put skip list('FAIL r =', r);
    end;
    s = 41;
    call bump(s);
    if s ^= 42 then do;
       fails = fails + 1;
       put skip list('FAIL s =', s);
    end;
    if fails = 0 then put skip list('PASS multimod_onedriver');
    else put skip list('FAIL multimod_onedriver: fails =', fails);
 end multimain;
EOF

$PLIC "$OUT.lib.pli" "$OUT.main.pli" -o "$OUT" || { echo "FAIL multimod_onedriver: compile+link"; exit 1; }
"$OUT"
exit 0
