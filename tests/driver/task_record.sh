#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/task_record.sh — a task that waits for a record must not
# stall the worker it runs on (C4).
#
# The program starts two tasks: one reads a record from a FIFO, the other
# sleeps and then writes a mark file. The FIFO has a writer but no data,
# so the reader waits inside READ. Without file-read parking that wait
# holds the worker, and the ticker never gets to write its mark.
#
# The driver keeps the write end open, polls for the mark while the reader
# waits, then sends the record and checks that the reader saw it.
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=./tests/driver/out/task_record
FIFO=./tests/driver/out/task_record.fifo
MARK=./tests/driver/out/task_record_ticker.mark
LOG=$OUT.log
mkdir -p tests/driver/out

cat > "$OUT.pli" <<'EOF'
 /* rules (79),(112): a record READ must park the task, not hold the
    worker; the ticker has to finish while the reader waits for the
    record to arrive on the FIFO. */
 tst: procedure options(main);
    declare e1 event;
    declare e2 event;
    call ticker event(e2);
    call reader event(e1);
    wait(e1);
    put skip list('PASS task_record');
    reader: procedure;
       declare f file;
       declare v char(8);
       open file(f) record sequential input
          title('task_record.fifo');
       read file(f) into(v);
       close file(f);
       if v ^= 'record42' then put skip list('FAIL value', v);
    end reader;
    ticker: procedure;
       declare g file;
       delay(500);
       open file(g) title('task_record_ticker.mark') output;
       put file(g) skip list('ticker-done');
       close file(g);
    end ticker;
 end tst;
EOF

$PLI "$OUT.pli" -o "$OUT" || { echo "FAIL task_record: compile"; exit 1; }

rm -f "$FIFO" "$MARK" "$LOG"
if ! command -v mkfifo >/dev/null 2>&1 || ! mkfifo "$FIFO"; then
   echo "SKIP task_record: mkfifo is not available"
   exit 0
fi

# Hold the write end open so OPEN returns and READ waits for a record.
( exec 3>"$FIFO"; sleep 30 ) &
holder=$!

# One worker: with more, another worker runs the ticker and the stall
# never shows.
( cd tests/driver/out && PLI_TASK_WORKERS=1 ./task_record ) >"$LOG" 2>&1 &
pid=$!

# Poll for the mark: the ticker may only run while the reader waits.
i=0
spot=0
while [ $i -lt 20 ]; do
   if [ -f "$MARK" ]; then
      spot=1
      break
   fi
   sleep 1
   i=$((i + 1))
done

if [ $spot -eq 0 ]; then
   echo "FAIL task_record: the ticker never finished (READ blocked the worker)"
   kill $pid 2>/dev/null
   wait $pid 2>/dev/null
   kill $holder 2>/dev/null
   wait $holder 2>/dev/null
   rm -f "$FIFO"
   exit 1
fi

# The reader is still parked: send the record and let the task finish.
( printf 'record42' > "$FIFO" ) &
feeder=$!
i=0
while [ $i -lt 10 ] && kill -0 $pid 2>/dev/null; do
   sleep 1
   i=$((i + 1))
done
if kill -0 $pid 2>/dev/null; then
   echo "FAIL task_record: the reader did not finish after the record arrived"
   kill $pid 2>/dev/null
   wait $pid 2>/dev/null
   kill $feeder 2>/dev/null
   wait $feeder 2>/dev/null
   kill $holder 2>/dev/null
   wait $holder 2>/dev/null
   rm -f "$FIFO"
   exit 1
fi
wait $pid 2>/dev/null
rc=$?
kill $feeder 2>/dev/null
wait $feeder 2>/dev/null
kill $holder 2>/dev/null
wait $holder 2>/dev/null
rm -f "$FIFO"

if [ $rc -ne 0 ] || ! grep -q 'PASS task_record' "$LOG" \
   || grep -q 'FAIL' "$LOG"; then
   echo "FAIL task_record: the program did not finish cleanly"
   cat "$LOG"
   exit 1
fi
echo "PASS task_record"
exit 0
