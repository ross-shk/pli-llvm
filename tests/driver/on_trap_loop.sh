#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/on_trap_loop.sh — a condition that traps inside a loop must not
# grow the stack. The ON-unit dispatch used to allocate its context slot in the
# trap block, so every trap leaked an alloca; a loop trapping often enough
# overflowed the stack at -O0/-O1/-O3 (the optimizer only hid it at -O2).
# The witness traps a million times at each optimization level and must
# produce the same answer everywhere.
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=tests/driver/out/on_trap_loop
mkdir -p tests/driver/out

cat > "$OUT.pli" <<'EOF'
on_trap_loop: procedure options(main);
   declare (i, d, q) fixed bin(31);
   declare (s, hits) fixed bin(63);
   on zerodivide begin;
      hits = hits + 1;
      return;
   end;
   s = 0;
   hits = 0;
   do i = 1 to 1000000;
      d = mod(i, 1);
      q = 1000000 / d;
      s = s + q;
   end;
   put skip list(s);
   put skip list(hits);
end on_trap_loop;
EOF

fail() { echo "FAIL on-trap-loop: $1"; exit 1; }

# The reference: the program must trap on every iteration and resume with 0.
ref=""
for opt in -O0 -O1 -O2 -O3; do
  if ! $PLI "$opt" "$OUT.pli" -o "$OUT.exe" 2>"$OUT.err"; then
    fail "$opt compile failed: $(cat "$OUT.err")"
  fi
  out=$("$OUT.exe" 2>&1)
  rc=$?
  [ "$rc" -eq 0 ] || fail "$opt exited $rc (a trap loop must not crash)"
  # The resume value is 0 and every iteration trapped.
  if [ "$out" != "0
1000000" ]; then
    fail "$opt wrong output: $out"
  fi
  if [ -z "$ref" ]; then
    ref="$out"
  elif [ "$out" != "$ref" ]; then
    fail "$opt output differs from -O0: '$out' vs '$ref'"
  fi
done

echo "PASS on-trap-loop"
exit 0
