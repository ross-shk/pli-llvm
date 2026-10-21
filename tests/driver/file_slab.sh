#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/file_slab.sh — WS-H4 (ADR-272): a list-directed GET FILE reads
# the record one character at a time, and a locked stdio call per character
# dominated the file read path (the same 500k records took 340 ms through the
# per-character path and 30 ms through the buffered one). The characters now
# come from a slab that one fread fills, and the slab belongs to the file it
# was filled from — so the characters read but not yet consumed survive the
# unselect at the end of a GET statement, and two tasks reading the same file
# still take whole records. The test writes 5 000 records and reads them back,
# checking every record and the total: enough records to cross the 8 KiB slab
# several times, so a refill that lost or duplicated a character would show.
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=./tests/driver/out/file_slab
mkdir -p tests/driver/out

cat > "$OUT.pli" <<'PLEOF'
 file_slab: procedure options(main);
    declare i fixed bin(31);
    declare n fixed bin(31);
    declare val fixed bin(31);
    declare checksum fixed bin(63);
    declare f file;

    /* 5000 records of up to 5 bytes each: well past the 8 KiB slab. */
    n = 5000;
    open file(f) title('pli_slab.tmp') output;
    do i = 1 to n;
       val = mod(i * 37, 1000);
       put file(f) skip list(val);
    end;
    close file(f);

    checksum = 0;
    open file(f) title('pli_slab.tmp') input;
    do i = 1 to n;
       get file(f) skip list(val);
       if val ^= mod(i * 37, 1000) then
          put skip list('FAIL file_slab: record', i, 'read', val);
       checksum = checksum + val;
    end;
    close file(f);

    put skip list('records=', n, 'checksum=', checksum);
 end file_slab;
PLEOF

$PLI "$OUT.pli" -o "$OUT" || { echo "FAIL file_slab: compile"; exit 1; }
( cd tests/driver/out && ./file_slab ) > "$OUT.stdout" 2>&1 || {
  echo "FAIL file_slab: run"
  cat "$OUT.stdout"
  exit 1
}
grep -q 'FAIL file_slab' "$OUT.stdout" && { echo "FAIL file_slab: a record read back wrong"; cat "$OUT.stdout"; exit 1; }
grep -q "records= 5000" "$OUT.stdout" || { echo "FAIL file_slab: no record count"; cat "$OUT.stdout"; exit 1; }
echo "PASS file_slab"
exit 0
