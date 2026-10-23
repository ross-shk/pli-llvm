#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/ext_async_test.sh — async CALL of an external entry procedure
# (ADR-291). The CHAR(*) parameter checks the hidden length argument that
# the async wrapper must pass, matching the synchronous external ABI.
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=./tests/driver/out/ext_async
mkdir -p tests/driver/out

cat > "$OUT.lib.pli" <<'EOF'
 /* SPDX-License-Identifier: Apache-2.0 */
 extlib: procedure(x, s);
    declare x fixed binary(31);
    declare s char(*);
    x = 42;
    s = 'OK';
 end extlib;
EOF

cat > "$OUT.main.pli" <<'EOF'
 /* SPDX-License-Identifier: Apache-2.0 */
 /* rules (34),(79),(82): external entry-procedure CALL with EVENT. */
 ext_async: procedure options(main);
    declare ev event;
    declare x fixed binary(31);
    declare s char(8);
    declare extlib entry (fixed binary(31), char(*)) external;

    x = 0;
    s = '';
    call extlib(x, s) event(ev);
    wait(ev);

    if x = 42 then
       if s = 'OK' then
          put skip list('PASS ext_async');
       else
          put skip list('FAIL ext_async buffer', s);
    else
       put skip list('FAIL ext_async scalar', x);
 end ext_async;
EOF

$PLI "$OUT.main.pli" "$OUT.lib.pli" -o "$OUT" || { echo "FAIL ext_async: compile"; exit 1; }

rc=0
for model in fiber os; do
   for workers in 1 4; do
      out=$(PLI_TASK_MODEL=$model PLI_TASK_WORKERS=$workers ./"$OUT" 2>&1)
      status=$?
      if [ $status -ne 0 ] || echo "$out" | grep -q '^FAIL'; then
         echo "FAIL ext_async: $model/$workers (exit $status)"
         echo "$out"
         rc=1
      elif echo "$out" | grep -q 'PASS ext_async'; then
         echo "PASS ext_async: $model/$workers"
      else
         echo "FAIL ext_async: $model/$workers (no PASS)"
         echo "$out"
         rc=1
      fi
   done
done
exit $rc
