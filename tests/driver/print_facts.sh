#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/print_facts.sh — --print-facts dumps the compiler's PL/I-specific
# decisions (ADR-260): the lowering policy, the check state, and the path each
# built-in call site took. The dump must follow the flags: the built-in path
# changes with --experimental-lowering, the check state with --size-checks.
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=tests/driver/out/print_facts
mkdir -p tests/driver/out

cat > "$OUT.pli" <<'EOF'
pfacts: procedure options(main);
   declare s char(8) varying;
   declare x fixed bin(31);
   s = 'hello';
   x = index(s, 'll');
   put skip list(x);
end pfacts;
EOF

fail() { echo "FAIL print-facts: $1"; exit 1; }

# --- the default dump has the module facts and the procedure block ---------
auto=$($PLI --print-facts "$OUT.pli" 2>&1) || fail "default dump failed"
echo "$auto" | grep -q '^; pli facts$' || fail "no facts header"
echo "$auto" | grep -q '^  lowering: auto$' || fail "no lowering line"
echo "$auto" | grep -q '^  checks: .*size off' || fail "no checks line"
echo "$auto" | grep -q '^proc PFACTS$' || fail "no procedure block"
echo "$auto" | grep -q '^  builtin index -> ' || fail "no builtin line"

# --- the built-in path follows --experimental-lowering ----------------------
rt=$($PLI --print-facts --experimental-lowering=runtime "$OUT.pli" 2>&1) || fail "runtime dump failed"
echo "$rt" | grep -q '^  lowering: runtime$' || fail "runtime lowering line"
echo "$rt" | grep -q '^  builtin index -> runtime' || fail "runtime did not take the library path"

ll=$($PLI --print-facts --experimental-lowering=llvm "$OUT.pli" 2>&1) || fail "llvm dump failed"
echo "$ll" | grep -q '^  lowering: llvm$' || fail "llvm lowering line"
echo "$ll" | grep -q '^  builtin index -> llvm' || fail "llvm did not take the inline path"

# --- the check state follows the flags --------------------------------------
on=$($PLI --print-facts --size-checks --subscript-checks "$OUT.pli" 2>&1) || fail "checks dump failed"
echo "$on" | grep -q '^  checks: .*size on' || fail "size checks not reported on"
echo "$on" | grep -q '^  checks: .*subscript on' || fail "subscript checks not reported on"

# --- it is a terminal mode: no output file is produced ----------------------
rm -f "$OUT.ll"
$PLI --print-facts -emit-llvm "$OUT.pli" -o "$OUT.ll" >/dev/null 2>&1
[ -f "$OUT.ll" ] && fail "--print-facts still wrote an output file"

# --- the effect and control verdicts follow the body (ADR-264) --------------
# A handled condition lets a handler run inside the raising procedure's frame,
# so the handler's writes reach the raiser's callers too.
cat > "$OUT.on.pli" <<'EOF'
pfon: procedure options(main);
   declare v fixed bin(31);
   on size;
   call outer(v);
   put skip list(v);
 outer: procedure(x);
   declare x fixed bin(31);
   x = x + 1;
 end outer;
end pfon;
EOF
sed '/on size;/d' "$OUT.on.pli" > "$OUT.free.pli"

# The block of one procedure in a facts dump.
block() { printf '%s\n' "$1" | awk -v p="proc $2" '$0 == p { f = 1; next } /^proc / { f = 0 } f'; }

handled=$($PLI --print-facts --size-checks "$OUT.on.pli" 2>&1) || fail "handler dump failed"
block "$handled" OUTER | grep -q 'writes memory outside its arguments' ||
  fail "a handled condition did not reach the caller"
block "$handled" OUTER | grep -q 'control:' &&
  fail "a procedure that can dispatch to a handler still claimed a control verdict"

plain=$($PLI --print-facts --size-checks "$OUT.free.pli" 2>&1) || fail "plain dump failed"
block "$plain" OUTER | grep -q 'control: nounwind' ||
  fail "no control verdict for a procedure with no handler in scope"

echo "PASS print-facts"
exit 0
