#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/task_hardening_matrix.sh — the hardening tasking tests
# under every backend (fiber/1, fiber/4, os/1, os/4), like task_matrix.sh.
# Tests that write scratch files (task_file_conds, task_input,
# task_output) stay out: this driver runs while the hardening group does,
# and the fixed scratch paths would collide.
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=tests/driver/out/task_hardening_matrix
mkdir -p "$OUT"

rc=0
for t in task_nested_wait task_nested_finish task_ctl_reuse task_ctl_reuse_multi \
         task_on_isolation task_on_combo task_alloc_combo; do
  $PLI "tests/hardening/$t.pli" -o "$OUT/$t" || { echo "FAIL $t: compile"; rc=1; continue; }
  for model in fiber os; do
    for workers in 1 4; do
      out=$(PLI_TASK_MODEL=$model PLI_TASK_WORKERS=$workers "./$OUT/$t" 2>&1)
      status=$?
      if [ $status -ne 0 ] || echo "$out" | grep -q '^FAIL' ||
         ! echo "$out" | grep -q "PASS $t"; then
        echo "FAIL $t: $model/$workers (exit $status)"
        echo "$out"
        rc=1
      else
        echo "PASS $t: $model/$workers"
      fi
    done
  done
done
exit $rc
