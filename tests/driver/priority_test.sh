#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/priority_test.sh — PRIORITY on every backend (fiber/1,
# fiber/4, os/1, os/4). Values are checked on all four; ordering is checked
# on fiber/1 only because the OS model does not schedule by priority
# (ADR-291).
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=./tests/driver/out/priority_test
mkdir -p tests/driver/out

cat > "$OUT.pli" <<'EOF'
 /* SPDX-License-Identifier: Apache-2.0 */
 /* rules (79),(82): PRIORITY values on every backend (ADR-291). */
 priority_values: procedure options(main);
    declare ev event;
    declare tk task;
    declare val fixed binary(15);
    declare ok fixed binary(31);

    ok = 1;
    call worker priority(10) event(ev) task(tk);
    wait(ev);

    val = priority(tk);
    if val ^= 10 then do;
       put skip list('FAIL priority spawn', val);
       ok = 0;
    end;

    priority(tk) = 7;
    val = priority(tk);
    if val ^= 7 then do;
       put skip list('FAIL priority set/read', val);
       ok = 0;
    end;

    priority(tk) = -3;
    val = priority(tk);
    if val ^= -3 then do;
       put skip list('FAIL priority negative', val);
       ok = 0;
    end;

    if ok then
       put skip list('PASS priority_values');
    else
       put skip list('FAIL priority_values');

    worker: procedure;
    end worker;
 end priority_values;
EOF

cat > "$OUT.order.pli" <<'EOF'
 /* SPDX-License-Identifier: Apache-2.0 */
 /* rule (79): a higher-priority task spawned first still runs before a
    lower-priority task spawned second on a single fiber worker (ADR-291). */
 priority_order: procedure options(main);
    declare e1 event;
    declare e2 event;
    declare seq fixed binary(31);
    declare order(2) fixed binary(31);

    seq = 0;
    order(1) = 0;
    order(2) = 0;

    /* High first, low second: plain LIFO would run low first. */
    call high priority(10) event(e1);
    call low priority(-10) event(e2);
    wait(e1, e2);

    if order(1) = 1 then
       if order(2) = 2 then
          put skip list('PASS priority_order');
       else
          put skip list('FAIL priority_order second', order(2));
    else
       put skip list('FAIL priority_order first', order(1));

    high: procedure;
       seq = seq + 1;
       order(seq) = 1;
    end high;

    low: procedure;
       seq = seq + 1;
       order(seq) = 2;
    end low;
 end priority_order;
EOF

$PLI "$OUT.pli" -o "$OUT" || { echo "FAIL priority_test: compile values"; exit 1; }
$PLI "$OUT.order.pli" -o "$OUT.order" || { echo "FAIL priority_test: compile order"; exit 1; }

rc=0
for model in fiber os; do
   for workers in 1 4; do
      out=$(PLI_TASK_MODEL=$model PLI_TASK_WORKERS=$workers ./"$OUT" 2>&1)
      status=$?
      if [ $status -ne 0 ] || echo "$out" | grep -q '^FAIL'; then
         echo "FAIL priority_values: $model/$workers (exit $status)"
         echo "$out"
         rc=1
      elif echo "$out" | grep -q 'PASS priority_values'; then
         echo "PASS priority_values: $model/$workers"
      else
         echo "FAIL priority_values: $model/$workers (no PASS)"
         echo "$out"
         rc=1
      fi
   done
done

# Ordering is only defined on the fiber model with one worker.
out=$(PLI_TASK_MODEL=fiber PLI_TASK_WORKERS=1 ./"$OUT.order" 2>&1)
status=$?
if [ $status -ne 0 ] || echo "$out" | grep -q '^FAIL'; then
   echo "FAIL priority_order: fiber/1 (exit $status)"
   echo "$out"
   rc=1
elif echo "$out" | grep -q 'PASS priority_order'; then
   echo "PASS priority_order: fiber/1"
else
   echo "FAIL priority_order: fiber/1 (no PASS)"
   echo "$out"
   rc=1
fi

exit $rc
