#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/task_stdin.sh — a task that waits for input must not stall
# the worker it runs on (ADR-238).
#
# The program starts two tasks: one reads standard input, and the other
# finishes. The input is the interesting part: a blocking read holds the
# OS thread, so without the input broker the worker that took the reader
# could not run the other task, and the ticker would not finish until
# the input arrived.
#
# The driver keeps the pipe open, so the reader waits for a second number
# that never comes, and checks whether the ticker has finished. The ticker
# writes its own file and closes it, so the driver sees the progress while
# the program is still running.
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=./tests/driver/out/task_stdin
mkdir -p tests/driver/out

cat > "$OUT.pli" <<'EOF'
 /* rules (79),(82): a task that waits for input must not stall the
    worker: the other task has to finish while the reader waits. The
    program waits for an event that never comes, so it stays alive and
    the ticker's mark is the only sign of life. */
 tst: procedure options(main);
    declare e1 event;
    declare e2 event;
    call ticker event(e2);
    call reader event(e1);
    wait(e1);
    put skip list('the reader finished');
    reader: procedure;
       declare x fixed bin(31);
       declare y fixed bin(31);
       get list (x);
       get list (y);   /* the pipe has one number: this one waits */
    end reader;
    ticker: procedure;
       declare i fixed bin(31);
       declare g file;
       do i = 1 to 200000;
       end;
       open file(g) title('tests/driver/out/task_stdin_ticker.mark') output;
       put file(g) skip list('ticker-done');
       close file(g);
    end ticker;
 end tst;
EOF

$PLI "$OUT.pli" -o "$OUT" || { echo "FAIL task_stdin: compile"; exit 1; }

rm -f tests/driver/out/task_stdin_ticker.mark
# One worker: with more, another worker steals the ticker and the stall
# never shows. The pipe stays open, so the reader waits for a second
# number that never comes.
PLI_TASK_WORKERS=1
export PLI_TASK_WORKERS
( printf '5\n'; sleep 30 ) | ./"$OUT" > /dev/null 2>&1 &
pid=$!

# Poll for the mark: the test suite runs the cases in parallel, so how long
# the ticker takes depends on the machine.
i=0
while [ $i -lt 20 ]; do
   if [ -f tests/driver/out/task_stdin_ticker.mark ]; then
      echo "PASS task_stdin"
      rc=0
      break
   fi
   sleep 1
   i=$((i + 1))
done
if [ $i -ge 20 ]; then
   echo "FAIL task_stdin: the ticker never finished (the input blocked the worker)"
   rc=1
fi
kill $pid 2>/dev/null
wait $pid 2>/dev/null
exit $rc
