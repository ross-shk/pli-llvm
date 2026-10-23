#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/task_matrix.sh — run the tasking matrix under every backend
# (fiber/1, fiber/4, os/1, os/4). The program lives in
# tests/core/task_matrix.pli so the core suite and this matrix cannot drift.
# PLI_TASK_MODEL=os opts into the legacy OS-thread model (ADR-298).
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=./tests/driver/out/task_matrix
mkdir -p tests/driver/out

$PLI tests/core/task_matrix.pli -o "$OUT" || { echo "FAIL task_matrix: compile"; exit 1; }

rc=0
for model in fiber os; do
   for workers in 1 4; do
      out=$(PLI_TASK_MODEL=$model PLI_TASK_WORKERS=$workers ./"$OUT" 2>&1)
      status=$?
      if [ $status -ne 0 ]; then
         echo "FAIL task_matrix: $model/$workers (exit $status)"
         echo "$out"
         rc=1
      elif echo "$out" | grep -q '^FAIL'; then
         echo "FAIL task_matrix: $model/$workers"
         echo "$out"
         rc=1
      elif echo "$out" | grep -q 'PASS task_matrix'; then
         echo "PASS task_matrix: $model/$workers"
      else
         echo "FAIL task_matrix: $model/$workers (no PASS)"
         echo "$out"
         rc=1
      fi
   done
done
exit $rc
