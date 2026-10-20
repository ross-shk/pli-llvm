#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/uninit.sh — --warn-unset reports a read that a path can reach
# before the variable is set (WS-A5, ADR-266). The check is opt-in, so the
# default build stays silent; storage the language initializes (CONTROLLED,
# BASED, DEFINED, INIT) and a variable whose address escapes never warn.
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=tests/driver/out/uninit
mkdir -p tests/driver/out

fail() { echo "FAIL uninit: $1"; exit 1; }

# How many "used before set" warnings a program produces.
warns() { $PLI "$1" -fsyntax-only --warn-unset 2>&1 | grep -c "warning: variable"; }

# --- one read of a variable nothing stored to -------------------------------
cat > "$OUT.unset.pli" <<'EOF'
nu_unset: procedure options(main);
   declare a fixed bin(31);
   declare b fixed bin(31);
   b = a + 1;
   put skip list(b);
end nu_unset;
EOF

# The check is off unless asked for.
out=$($PLI "$OUT.unset.pli" -fsyntax-only 2>&1) || fail "default build failed"
echo "$out" | grep -q "warning: variable" && fail "--warn-unset is not opt-in"

out=$($PLI "$OUT.unset.pli" -fsyntax-only --warn-unset 2>&1) ||
  fail "a warning must not fail the build"
echo "$out" | grep -q "warning: variable 'A' may be used before it is set" ||
  fail "no warning for the unset variable"

# --- set before use, and set on both branches, are clean --------------------
cat > "$OUT.ok.pli" <<'EOF'
nu_ok: procedure options(main);
   declare c fixed bin(31);
   declare a fixed bin(31);
   c = 1;
   if c > 0 then a = 1; else a = 2;
   put skip list(a);
end nu_ok;
EOF
[ "$(warns "$OUT.ok.pli")" -eq 0 ] || fail "a variable set on every path still warned"

# --- a store on one branch is not a store on the other ---------------------
cat > "$OUT.branch.pli" <<'EOF'
nu_branch: procedure options(main);
   declare c fixed bin(31);
   declare a fixed bin(31);
   c = 1;
   if c > 0 then a = 1;
   put skip list(a);
end nu_branch;
EOF
[ "$(warns "$OUT.branch.pli")" -eq 1 ] || fail "a one-branch store did not warn"

# --- a branch that returns leaves the rest of the path knowing more --------
cat > "$OUT.ret.pli" <<'EOF'
nu_ret: procedure options(main);
   declare c fixed bin(31);
   declare a fixed bin(31);
   c = 1;
   if c > 0 then a = 1; else return;
   put skip list(a);
end nu_ret;
EOF
[ "$(warns "$OUT.ret.pli")" -eq 0 ] || fail "a returning else branch was treated as a fall-through"

# --- a loop body reads what an earlier iteration stored --------------------
cat > "$OUT.loop.pli" <<'EOF'
nu_loop: procedure options(main);
   declare i fixed bin(31);
   declare a fixed bin(31);
   do i = 1 to 3;
      a = a + 1;
   end;
   put skip list(a);
end nu_loop;
EOF
[ "$(warns "$OUT.loop.pli")" -eq 1 ] || fail "an unset loop accumulator did not warn"

# --- storage that is initialized for us, or written where we cannot see ----
cat > "$OUT.storage.pli" <<'EOF'
nu_storage: procedure options(main);
   declare c fixed bin(31) controlled;
   declare p pointer;
   declare b fixed bin(31) based(p);
   declare s char(8);
   declare d char(8) defined(s);
   allocate c;
   put skip list(c);
   put skip list(b);
   put skip list(d);
end nu_storage;
EOF
[ "$(warns "$OUT.storage.pli")" -eq 0 ] || fail "CONTROLLED/BASED/DEFINED storage warned"

cat > "$OUT.init.pli" <<'EOF'
nu_init: procedure options(main);
   declare v fixed bin(31) init(7);
   put skip list(v);
end nu_init;
EOF
[ "$(warns "$OUT.init.pli")" -eq 0 ] || fail "an INIT variable warned"

# --- a by-reference argument, GET into, and ADDR all count as stores ------
cat > "$OUT.calls.pli" <<'EOF'
nu_calls: procedure options(main);
   declare a fixed bin(31);
   declare g fixed bin(31);
   declare p pointer;
   call setit(a);
   get list(g);
   p = addr(a);
   put skip list(a);
   put skip list(g);
 setit: procedure(x);
   declare x fixed bin(31);
   x = 1;
 end setit;
end nu_calls;
EOF
[ "$(warns "$OUT.calls.pli")" -eq 0 ] || fail "a store the walk cannot see still warned"

# --- a subscript target and a structure member are not whole variables ----
cat > "$OUT.agg.pli" <<'EOF'
nu_agg: procedure options(main);
   declare a(3) fixed bin(31);
   declare 1 s,
     2 m fixed bin(31);
   a(1) = 1;
   s.m = 1;
   put skip list(a(2));
   put skip list(s.m);
end nu_agg;
EOF
[ "$(warns "$OUT.agg.pli")" -eq 0 ] || fail "an array element or member warned"

echo "PASS uninit"
exit 0
