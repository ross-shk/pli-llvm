#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/stream_opts.sh — COPY and LINE stream options (rule (105)):
# PUT COPY echoes the remainder of the current input record to the output;
# GET LINE(n) skips n input lines before the statement reads.
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=./tests/driver/out/stream_opts
mkdir -p tests/driver/out

cat > "$OUT.pli" <<'EOF'
 opts: procedure options(main);
    declare a char(5);
    declare b char(5);
    /* Read five chars, then echo the rest of the line. */
    get edit(a) (a(5));
    put copy;
    /* The echo consumed the first newline; skip one line. */
    get line(1) edit(b) (a(5));
    if a = 'abcde' & b = 'HELLO' then
       put skip list('PASS stream_opts');
    else do;
       put skip list('FAIL stream_opts: got >', a, '< >', b, '<');
       stop;
    end;
 end opts;
EOF

$PLI "$OUT.pli" -o "$OUT" || { echo "FAIL stream_opts: compile"; exit 1; }
result=$(printf 'abcdeREST\nSKIP1\nHELLO\n' | "$OUT")
printf '%s\n' "$result"
# COPY must echo the input remainder before the program's own output.
printf '%s\n' "$result" | grep -q 'REST' || { echo "FAIL stream_opts: COPY echo missing"; exit 1; }
printf '%s\n' "$result" | grep -q 'PASS stream_opts' || { echo "FAIL stream_opts: program did not pass"; exit 1; }
exit 0
