#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/emitted_asan.sh — -fsanitize=address instruments the code the
# compiler emits (B13, ADR-341). The B0 runs build the runtime from source
# under a sanitizer; this test is the other half: irgen's own memory accesses
# (stack arrays, globals) and the runtime allocator's heap blocks.
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=tests/driver/out/emitted_asan
mkdir -p "$OUT"

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    echo "SKIP: the AddressSanitizer runtime is Unix-only"
    exit 0
    ;;
esac

# compile_asan <src> <exe>; a toolchain without compiler-rt is a counted skip.
compile_asan() {
  if $PLI -fsanitize=address -O2 "$1" -o "$2" >"$OUT/$3.compile" 2>&1; then
    return 0
  fi
  if grep -Eq 'needs the (compiler-rt ASan runtime|dynamic C runtime)' "$OUT/$3.compile"; then
    echo "SKIP: no AddressSanitizer runtime in this toolchain"
    exit 0
  fi
  echo "FAIL emitted_asan: $1 does not compile with -fsanitize=address"
  tail -5 "$OUT/$3.compile"
  exit 1
}

# A stack array with a subscript one past the last element: the PL/I-level
# check is off at -O2 (--no-subscript default), so ASan must catch the store.
cat > "$OUT/stack_oob.pli" <<'EOF'
 asan_stack: proc options(main);
   dcl a(256) fixed bin(31);
   dcl i fixed bin(31);
   i = 257;
   a(i) = 7;
   put skip list('no report');
 end asan_stack;
EOF

# The same program in bounds, and a heap array with a one-past store.
cat > "$OUT/in_bounds.pli" <<'EOF'
 asan_ok: proc options(main);
   dcl a(256) fixed bin(31);
   dcl i fixed bin(31);
   i = 256;
   a(i) = 7;
   if a(i) = 7 then put skip list('PASS');
 end asan_ok;
EOF

cat > "$OUT/heap_oob.pli" <<'EOF'
 asan_heap: proc options(main);
   dcl p pointer;
   dcl b(64) fixed bin(31) based(p);
   dcl i fixed bin(31);
   allocate b set(p);
   i = 65;
   b(i) = 1;
   put skip list('no report');
 end asan_heap;
EOF

compile_asan "$OUT/stack_oob.pli" "$OUT/stack_oob" stack_oob
compile_asan "$OUT/in_bounds.pli" "$OUT/in_bounds" in_bounds
compile_asan "$OUT/heap_oob.pli" "$OUT/heap_oob" heap_oob

# expect_report <exe> <kind>: a non-zero exit whose report names the kind.
expect_report() {
  out=$(ASAN_OPTIONS=detect_leaks=0 "$1" 2>&1)
  rc=$?
  case "$out" in
    *"ERROR: AddressSanitizer: $2"*)
      ;;
    *)
      echo "FAIL emitted_asan: $1 gave no $2 report (exit $rc)"
      echo "$out" | head -8
      exit 1
      ;;
  esac
  if [ "$rc" -eq 0 ]; then
    echo "FAIL emitted_asan: $1 reported $2 but exited 0"
    exit 1
  fi
}

expect_report "$OUT/stack_oob" stack-buffer-overflow
expect_report "$OUT/heap_oob" heap-buffer-overflow

# The in-bounds control proves the instrumentation adds no false positive.
out=$(ASAN_OPTIONS=detect_leaks=0 "$OUT/in_bounds" 2>&1)
case "$out" in
  *AddressSanitizer*)
    echo "FAIL emitted_asan: the in-bounds program reported a finding"
    echo "$out" | head -8
    exit 1
    ;;
  *PASS*) ;;
  *)
    echo "FAIL emitted_asan: the in-bounds program did not pass: $out"
    exit 1
    ;;
esac

# Without the option the same overflow is silent (exit 0): the option, not
# the program, is what changes the outcome.
if ! $PLI -O2 "$OUT/stack_oob.pli" -o "$OUT/stack_plain" >"$OUT/stack_plain.compile" 2>&1; then
  echo "FAIL emitted_asan: the unsanitized control does not compile"
  tail -5 "$OUT/stack_plain.compile"
  exit 1
fi
out=$("$OUT/stack_plain" 2>&1)
case "$out" in
  *"no report"*) ;;
  *)
    echo "FAIL emitted_asan: the unsanitized control did not run: $out"
    exit 1
    ;;
esac

# -c keeps the object (no runtime needed at this step).
if ! $PLI -fsanitize=address -c "$OUT/in_bounds.pli" -o "$OUT/in_bounds.o" \
     >"$OUT/compile_only.log" 2>&1; then
  echo "FAIL emitted_asan: -fsanitize=address -c failed"
  tail -5 "$OUT/compile_only.log"
  exit 1
fi

# Only the served sanitizer is accepted; both refusals must be explicit.
if $PLI -fsanitize=thread "$OUT/in_bounds.pli" -o "$OUT/thread" \
     >"$OUT/thread.log" 2>&1; then
  echo "FAIL emitted_asan: -fsanitize=thread was accepted"
  exit 1
fi
if ! grep -q 'only -fsanitize=address' "$OUT/thread.log"; then
  echo "FAIL emitted_asan: -fsanitize=thread gave no scope diagnostic"
  cat "$OUT/thread.log"
  exit 1
fi
if $PLI -fsanitize=address --flto=thin "$OUT/in_bounds.pli" -o "$OUT/lto" \
     >"$OUT/lto.log" 2>&1; then
  echo "FAIL emitted_asan: --flto accepted -fsanitize=address"
  exit 1
fi
if $PLI -fsanitize=address -emit-llvm "$OUT/in_bounds.pli" -o "$OUT/lto.ll" \
     >"$OUT/emit_ll.log" 2>&1; then
  echo "FAIL emitted_asan: -emit-llvm accepted -fsanitize=address"
  exit 1
fi
if ! grep -q 'stop before it' "$OUT/emit_ll.log"; then
  echo "FAIL emitted_asan: -emit-llvm gave no scope diagnostic"
  cat "$OUT/emit_ll.log"
  exit 1
fi

echo "SANITIZER: address"
echo "PASS emitted_asan"
