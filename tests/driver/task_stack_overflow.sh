#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/task_stack_overflow.sh - a task that runs out of stack says
# so, and does not hang (ADR-239).
#
# The tasks recurse until their stacks are full. The runtime must say
# "TASK: stack overflow (the task's stack ran out)" and exit with 134, the
# same way on Windows and on POSIX. Before ADR-239 the Windows fiber stack
# could not grow at all: the stack that ran out killed the process with a
# bare access violation, and a stack that grew past its first window did
# the same.
#
# Run twice: on one worker, where the fault is the main worker's, and on
# four, where the tasks are spread over the workers. The second run is the
# one that catches a report that only works on the thread that started the
# scheduler (the POSIX handler needs an alternate stack per thread).
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=./tests/driver/out/task_stack_overflow
mkdir -p tests/driver/out

cat > "$OUT.pli" <<'EOF'
 /* rules (79): a task that runs out of stack is reported, not left to
    die in a way the program cannot explain. Each task recurses until its
    stack is full; the runtime says so and stops the program. */
 sov: procedure options(main);
    declare ev(8) event;
    declare i fixed binary(31);
    declare total fixed binary(31);
    total = 0;
    do i = 1 to 8;
       call deep event(ev(i));
    end;
    wait(ev);
    put skip list('FAIL the tasks returned', total);
    deep: procedure recursive;
       declare a(1000) fixed binary(31);
       declare j fixed binary(31);
       do j = 1 to 1000;
          a(j) = j;
       end;
       call deep;
       /* The value is used, so the call is not a tail call: the frame
          stays, and the stack grows. */
       total = total + a(1);
    end deep;
 end sov;
EOF

$PLI "$OUT.pli" -o "$OUT" || { echo "FAIL task_stack_overflow: compile"; exit 1; }

# A small ceiling, so a task runs out of stack quickly. The test is about
# the fiber model's report: under PLI_TASK_MODEL=os a task is an OS thread,
# and the OS handles its stack overflow its own way.
PLI_TASK_STACK=131072
PLI_TASK_MODEL=fiber
export PLI_TASK_STACK PLI_TASK_MODEL

for workers in 1 4; do
   PLI_TASK_WORKERS=$workers
   export PLI_TASK_WORKERS
   "$OUT" > tests/driver/out/task_stack_overflow.out 2>&1
   rc=$?

   if [ $rc -ne 134 ]; then
      echo "FAIL task_stack_overflow: $workers worker(s): expected exit 134, got $rc"
      cat tests/driver/out/task_stack_overflow.out
      exit 1
   fi
   if ! grep -q "stack overflow" tests/driver/out/task_stack_overflow.out; then
      echo "FAIL task_stack_overflow: $workers worker(s): the runtime did not report the overflow"
      cat tests/driver/out/task_stack_overflow.out
      exit 1
   fi
done
echo "PASS task_stack_overflow"
exit 0
