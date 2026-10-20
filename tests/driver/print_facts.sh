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

# --- an ON unit makes the callee, and so its callers, opaque (ADR-264) ------
cat > "$OUT.chain.pli" <<'EOF'
pfchain: procedure options(main);
   declare v fixed bin(31);
   call helper(v);
 helper: procedure(x);
   declare x fixed bin(31);
   on size;
   x = x + 1;
 end helper;
end pfchain;
EOF
sed '/on size;/d' "$OUT.chain.pli" > "$OUT.chain2.pli"

chain=$($PLI --print-facts "$OUT.chain.pli" 2>&1) || fail "ON chain dump failed"
block "$chain" HELPER | grep -q 'establishes an ON unit' ||
  fail "the ON-bearing procedure kept an effect"
block "$chain" PFCHAIN | grep -q 'calls something whose effects are unknown' ||
  fail "the caller of an ON-bearing procedure kept a claim"

free=$($PLI --print-facts "$OUT.chain2.pli" 2>&1) || fail "plain chain dump failed"
block "$free" PFCHAIN | grep -q 'touches no memory' ||
  fail "the caller lost its claim without an ON unit"

# --- a name that escapes says so, not blaming a call it never makes ---------
cat > "$OUT.esc.pli" <<'EOF'
pfesc: procedure options(main);
   declare e entry (fixed bin(31)) variable;
   declare v fixed bin(31);
   e = pure;
   call e(v);
 pure: procedure(x);
   declare x fixed bin(31);
   x = x + 1;
 end pure;
end pfesc;
EOF
esc=$($PLI --print-facts "$OUT.esc.pli" 2>&1) || fail "escape dump failed"
block "$esc" PURE | grep -q 'its name escapes' ||
  fail "an escaped name blamed a call it does not make"

echo "PASS print-facts"
exit 0
