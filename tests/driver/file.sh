#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM
# tests/driver/file.sh — OPEN/CLOSE + FILE ( f ) (rules (100)-(103),(105)):
# PUT FILE(f) writes list-directed output into a named file and GET FILE(f)
# reads it back; the program verifies the values and prints PASS. It runs from
# the gitignored out/ dir so the data file does not pollute the repo root.
set -u
PLIC=./build/plic
OUT=./tests/driver/out/file
mkdir -p tests/driver/out

cat > "$OUT.pli" <<'EOF'
 file_test: procedure options(main);
    declare f file;
    declare a fixed bin(31);
    declare b fixed bin(31);
    declare c fixed bin(31);
    declare d fixed bin(31);
    declare fails fixed bin(31);
    fails = 0;
    a = 42;
    b = 7;
    open file(f) title('filetest.dat') output;
    put file(f) list(a, b);
    close file(f);
    open file(f) title('filetest.dat') input;
    get file(f) list(c, d);
    close file(f);
    if c ^= 42 then do;
       fails = fails + 1;
       put skip list('FAIL c');
    end;
    if d ^= 7 then do;
       fails = fails + 1;
       put skip list('FAIL d');
    end;
    if fails = 0 then put skip list('PASS file');
    else put skip list('FAIL file: fails =', fails);
 end file_test;
EOF

$PLIC "$OUT.pli" -o "$OUT" || { echo "FAIL file: compile"; exit 1; }
( cd tests/driver/out && ./file )
rm -f tests/driver/out/filetest.dat
exit 0
