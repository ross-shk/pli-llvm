#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM
# tests/driver/multimod.sh — multi-module PL/I programs (rules
# (2),(38),(42)): a library module provides external procedures,
# the MAIN module calls them across the link. Each unit compiles
# with -c; cc links the objects with libpli. It runs from the
# gitignored out/ dir so artifacts do not pollute the repo.
set -u
PLIC=./build/plic
CC=clang
RTLIB=./build/libpli.a
OUT=tests/driver/out/multimod
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
    if fails = 0 then put skip list('PASS multimod');
    else put skip list('FAIL multimod: fails =', fails);
 end multimain;
EOF

$PLIC "$OUT.lib.pli" -c -o "$OUT.lib.o" || { echo "FAIL multimod: lib compile"; exit 1; }
$PLIC "$OUT.main.pli" -c -o "$OUT.main.o" || { echo "FAIL multimod: main compile"; exit 1; }
$CC "$OUT.main.o" "$OUT.lib.o" "$RTLIB" -o "$OUT" || { echo "FAIL multimod: link"; exit 1; }
"$OUT"
exit 0
