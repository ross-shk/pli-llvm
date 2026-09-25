#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM
# tests/driver/record.sh — SEQUENTIAL RECORD files (112),(113):
# WRITE FILE(f) FROM(v) appends one fixed-size binary record and
# READ FILE(f) INTO(v) reads one back; the program round-trips
# FIXED, FLOAT, CHAR, and BIT values and prints PASS. It runs from
# the gitignored out/ dir so the data file does not pollute the repo.
set -u
PLIC=./build/plic
OUT=./tests/driver/out/record
mkdir -p tests/driver/out

cat > "$OUT.pli" <<'EOF'
 record_test: procedure options(main);
    declare f file;
    declare fails fixed bin(31);
    declare a fixed bin(31);
    declare b fixed bin(31);
    declare x float;
    declare s char(8);
    declare k bit(1);
    declare g fixed bin(31);
    declare h fixed bin(31);
    declare y float;
    declare t char(8);
    declare m bit(1);
    fails = 0;
    a = -42;
    b = 7;
    x = 3.5;
    s = 'hello';
    k = '1'b;
    open file(f) record sequential output
       title('recordtest.dat');
    write file(f) from(a);
    write file(f) from(b);
    write file(f) from(x);
    write file(f) from(s);
    write file(f) from(k);
    close file(f);
    open file(f) record sequential input
       title('recordtest.dat');
    read file(f) into(g);
    read file(f) into(h);
    read file(f) into(y);
    read file(f) into(t);
    read file(f) into(m);
    close file(f);
    if g ^= -42 then do;
       fails = fails + 1;
       put skip list('FAIL g');
    end;
    if h ^= 7 then do;
       fails = fails + 1;
       put skip list('FAIL h');
    end;
    if y ^= 3.5 then do;
       fails = fails + 1;
       put skip list('FAIL y');
    end;
    if t ^= 'hello' then do;
       fails = fails + 1;
       put skip list('FAIL t');
    end;
    if m ^= '1'b then do;
       fails = fails + 1;
       put skip list('FAIL m');
    end;
    if fails = 0 then put skip list('PASS record');
    else put skip list('FAIL record: fails =', fails);
 end record_test;
EOF

$PLIC "$OUT.pli" -o "$OUT" || { echo "FAIL record: compile"; exit 1; }
( cd tests/driver/out && ./record )
rm -f tests/driver/out/recordtest.dat
exit 0
