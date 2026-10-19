#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/task_output.sh — concurrent CALL ... TASK output (rule (79)):
# four tasks write fifty records each to SYSPRINT. The output path is
# serialised (ADR-233), so every line is a whole record; without it the
# records interleave and the lines come out garbled. The program itself
# prints only its records, so the driver checks them.
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=./tests/driver/out/task_output
mkdir -p tests/driver/out

cat > "$OUT.pli" <<'EOF'
 /* rules (79),(82): four tasks write 250 records each. */
 tout: procedure options(main);
    declare e1 event;
    declare e2 event;
    declare e3 event;
    declare e4 event;
    call worker(1) event(e1);
    call worker(2) event(e2);
    call worker(3) event(e3);
    call worker(4) event(e4);
    wait(e1, e2, e3, e4);
    worker: procedure(idx);
       declare idx fixed bin(31);
       declare j fixed bin(31);
       do j = 1 to 250;
          put skip list('task', idx, 'line', j);
       end;
    end worker;
 end tout;
EOF

$PLI "$OUT.pli" -o "$OUT" || { echo "FAIL task_output: compile"; exit 1; }
( cd tests/driver/out && ./task_output ) > "$OUT.stdout" || { echo "FAIL task_output: run"; exit 1; }

# every line must be one whole record, and all 1000 records must be there
total=$(wc -l < "$OUT.stdout")
whole=$(grep -c '^task [0-9][0-9]* line [0-9][0-9]*$' "$OUT.stdout")
if [ "$total" -ne 1000 ] || [ "$whole" -ne 1000 ]; then
   echo "FAIL task_output: lines=$total whole=$whole"
   exit 1
fi
echo "PASS task_output"
exit 0
