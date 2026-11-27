#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/blob_manifest.sh — Darwin runtime blob freshness (ADR-396): the
# checked-in platform/darwin-runtime blobs must match the runtime/ sources they
# were built from. Mirrors the CMake gate in CMakeLists.txt; run in CI (and the
# normal suite) so a routine runtime/ edit that forgot to regenerate blobs is
# caught without needing a fresh configure. Pure python3+shell, runs on every
# host; no run_tests.py skip-list entry needed.
set -u
ROOT=${ROOT:-.}
if [ ! -e "$ROOT/platform/darwin-runtime/SHA256SUMS" ]; then
  echo "SKIP: no Darwin runtime blobs vendored (bootstrap on macOS with scripts/gen_darwin_runtime.py)"
  exit 0
fi
HASH=$(python3 "$ROOT/scripts/runtime_source_hash.py" "$ROOT/runtime") \
  || { echo "FAIL: runtime_source_hash.py failed"; exit 1; }
EXPECT=$(cat "$ROOT/platform/darwin-runtime/SHA256SUMS")
if [ "$HASH" = "$EXPECT" ]; then
  echo "PASS blob_manifest"
else
  echo "FAIL: Darwin runtime blobs stale vs runtime/ sources; regenerate with scripts/gen_darwin_runtime.py on macOS"
  exit 1
fi
