#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/remarks.sh — the library-call remark (WS-K4, ADR-254): when
# the compiler hands a conversion to a library routine, it says so, so the
# user can find the hot spots. IBM has flagged every library-call conversion
# since 1991 (zos:23609-23610). The remark is off by default — it must not
# be noisy — and the same decision is reported by --print-facts.
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=tests/driver/out/remarks
mkdir -p tests/driver/out

fail() { echo "FAIL remarks: $1"; exit 1; }

# --- a conversion the library routine does: the picture conversion ---------
cat > "$OUT.lib.pli" <<'EOF'
rlib: procedure options(main);
   declare b char(20);
   declare v fixed decimal(9,2);
   v = 12.5;
   put string(b) edit(v) (P('ZZ9.99'));
   put skip list(b);
end rlib;
EOF

# --- the same conversion done by the compiler: a constant picture folds ----
cat > "$OUT.inline.pli" <<'EOF'
rinl: procedure options(main);
   declare b char(20);
   put string(b) edit(123.45) (P('ZZ9.99'));
   put skip list(b);
end rinl;
EOF

# --- the remark is off by default -------------------------------------------
quiet=$($PLI "$OUT.lib.pli" -o "$OUT.lib" 2>&1) || fail "the library build failed"
echo "$quiet" | grep -q 'remark:' && fail "the remark is on without the flag"

# --- with the flag, the library conversion is flagged -----------------------
lib=$($PLI --remark-library-calls "$OUT.lib.pli" -o "$OUT.lib" 2>&1) || fail "the flagged build failed"
echo "$lib" | grep -q 'remark: the library routine does this conversion' ||
  fail "the library-call conversion was not flagged"

# --- and the compiler's own conversion is not -------------------------------
inl=$($PLI --remark-library-calls "$OUT.inline.pli" -o "$OUT.inline" 2>&1) || fail "the inline build failed"
echo "$inl" | grep -q 'remark:' && fail "a folded conversion was flagged"

# --- the facts report the same decision, with the reason --------------------
facts=$($PLI --print-facts "$OUT.lib.pli" -o "$OUT.lib" 2>&1) || fail "the facts dump failed"
echo "$facts" | grep -q 'builtin picture -> runtime' ||
  fail "the facts do not report the picture path"
echo "$facts" | grep -q 'the picture conversion needs the library routine' ||
  fail "the facts do not report the reason"

# --- a remark does not change the program's behaviour -----------------------
$PLI --remark-library-calls "$OUT.lib.pli" -o "$OUT.lib" 2>/dev/null
$PLI "$OUT.lib.pli" -o "$OUT.plain" 2>/dev/null
a=$("$OUT.lib"); b=$("$OUT.plain")
[ "$a" = "$b" ] || fail "the remark changed the output"

echo "PASS remarks"
exit 0
