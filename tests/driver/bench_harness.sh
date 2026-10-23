#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/bench_harness.sh — the benchmark harness must refuse to time a
# run that crashed, timed out or printed the wrong output, and must not report
# the C-parity gate over a partial or mismatched corpus. Those rules live in
# benchmarks/bench_common.py and benchmarks/run_benchmarks.py; the tests are
# plain unittest cases that need no compiler build.
set -u
PY=${PYTHON:-python3}
command -v "$PY" >/dev/null 2>&1 || { echo "SKIP bench-harness (no python3)"; exit 0; }

OUT=tests/driver/out/bench_harness.log
mkdir -p tests/driver/out
if "$PY" -m unittest discover -s benchmarks/tests -p 'test_*.py' >"$OUT" 2>&1; then
  echo "PASS bench-harness"
  exit 0
fi
echo "FAIL bench-harness: $(tail -n 20 "$OUT")"
exit 1
