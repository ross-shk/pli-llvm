#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/record_option_state.sh — READ ... INTO on a sequential file.
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=./tests/driver/out/record_option_state
mkdir -p tests/driver/out

cat > "$OUT.pli" <<'EOF'
 record_option_state_driver: procedure options(main);
    declare f file;
    declare a fixed bin(31);
    declare b fixed bin(31);
    a = 42;
    open file(f) record sequential output
       title('record_option_state.dat');
    write file(f) from(a);
    close file(f);
    open file(f) record sequential input
       title('record_option_state.dat');
    read file(f) into(b);
    close file(f);
    if b ^= 42 then do;
       put skip list('FAIL record_option_state', b);
    end;
    else put skip list('PASS record_option_state');
 end record_option_state_driver;
EOF

$PLI "$OUT.pli" -o "$OUT" || { echo "FAIL record_option_state: compile"; exit 1; }
( cd tests/driver/out && ./record_option_state )
rm -f tests/driver/out/record_option_state.dat
exit 0
