#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/tasking_tsan.sh — the tasking runtime under ThreadSanitizer
# (B0). The runtime is built from source with TSan, so the scheduler, the
# worker queues and the task synchronisation are instrumented. Each program
# runs under the default fiber model and the legacy OS-thread model
# (PLI_TASK_MODEL=os), because the two use different scheduling paths; a
# race report fails, as does a program that stops printing PASS.
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=tests/driver/out/tasking_tsan
mkdir -p "$OUT"
. tests/driver/sanitizer_common.inc

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    echo "SKIP: the sanitizer runtime setup is Unix-only"
    exit 0
    ;;
esac

CC=$(san_cc)
if [ -z "$CC" ]; then
  echo "SKIP: no clang from the compiler's own LLVM (set LLVM_BINDIR)"
  exit 0
fi

SAN="-fsanitize=thread"
if ! san_build_runtime "$CC" "$SAN" "$OUT/rt"; then
  echo "FAIL tasking_tsan: the runtime did not compile under the sanitizer"
  tail -5 "$OUT/rt/"*.log 2>/dev/null
  exit 1
fi

# A single wait, a fan-out of a hundred concurrent tasks, and fiber reuse:
# together they drive work stealing, the park/unpark paths and the pool.
for t in task task_many task_reuse; do
  if ! $PLI --no-bitcode-runtime "tests/core/$t.pli" -c -o "$OUT/$t.o" \
       >"$OUT/$t.compile" 2>&1; then
    echo "FAIL tasking_tsan: $t did not compile"
    tail -5 "$OUT/$t.compile"
    exit 1
  fi
  if ! san_link "$CC" "$SAN" "$OUT/$t.o" "$OUT/rt" "$OUT/$t" 2>"$OUT/$t.link"; then
    if san_missing_runtime "$OUT/$t.link"; then
      echo "SKIP: no ThreadSanitizer runtime in this toolchain"
      exit 0
    fi
    echo "FAIL tasking_tsan: $t did not link"
    cat "$OUT/$t.link"
    exit 1
  fi

  for model in fiber os; do
    if [ "$model" = os ]; then
      out=$(PLI_TASK_MODEL=os "$OUT/$t" 2>&1)
    else
      out=$("$OUT/$t" 2>&1)
    fi
    case "$out" in
      *ThreadSanitizer*|*data\ race*)
        echo "FAIL tasking_tsan: $t ($model model) reported a race"
        echo "$out" | head -20
        exit 1
        ;;
    esac
    case "$out" in
      *PASS*) ;;
      *)
        echo "FAIL tasking_tsan: $t ($model model): $out"
        exit 1
        ;;
    esac
  done
done

echo "SANITIZER: thread"
echo "PASS tasking_tsan"
