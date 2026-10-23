#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/storage_asan.sh — the storage and overlay paths under
# AddressSanitizer and UndefinedBehaviorSanitizer (B0, QR2.3). The runtime
# is built from source with the sanitizers, so allocation, BASED overlays,
# CONTROLLED generations and DEFINED overlays are instrumented; the programs
# are the self-checking core tests for those paths. A finding is a report or
# a missing PASS, so a silently wrong value fails the test too.
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=tests/driver/out/storage_asan
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

SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
if ! san_build_runtime "$CC" "$SAN" "$OUT/rt"; then
  echo "FAIL storage_asan: the runtime did not compile under the sanitizer"
  tail -5 "$OUT/rt/"*.log 2>/dev/null
  exit 1
fi

# The self-checking storage, overlay and string programs; each must PASS.
for t in based_array based_heap_dyn based_locator_expr based_locator_sub \
         based_param controlled controlled_dyn controlled_reuse \
         controlled_varying defined defined_diff varying_nested; do
  if ! $PLI --no-bitcode-runtime "tests/core/$t.pli" -c -o "$OUT/$t.o" \
       >"$OUT/$t.compile" 2>&1; then
    echo "FAIL storage_asan: $t did not compile"
    tail -5 "$OUT/$t.compile"
    exit 1
  fi
  if ! san_link "$CC" "$SAN" "$OUT/$t.o" "$OUT/rt" "$OUT/$t" 2>"$OUT/$t.link"; then
    if san_missing_runtime "$OUT/$t.link"; then
      echo "SKIP: no AddressSanitizer/UndefinedBehaviorSanitizer runtime in this toolchain"
      exit 0
    fi
    echo "FAIL storage_asan: $t did not link"
    cat "$OUT/$t.link"
    exit 1
  fi

  out=$("$OUT/$t" 2>&1)
  case "$out" in
    *AddressSanitizer*|*UndefinedBehaviorSanitizer*|*runtime\ error:*)
      echo "FAIL storage_asan: $t reported a sanitizer finding"
      echo "$out" | head -20
      exit 1
      ;;
  esac
  case "$out" in
    *PASS*) ;;
    *)
      echo "FAIL storage_asan: $t: $out"
      exit 1
      ;;
  esac
done

echo "SANITIZER: address,undefined"
echo "PASS storage_asan"
