#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/effects_file.sh — WS-A2 (ADR-267): a module publishes the
# effects its external procedures promise, and the module that calls them
# attaches the promise to its declarations. The caller's optimizer cannot see
# the callee's body, so this description is all it has: without it the call
# keeps LLVM's default effects and cannot be hoisted out of a loop.
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=tests/driver/out/effects_file
mkdir -p tests/driver/out

fail() { echo "FAIL effects-file: $1"; exit 1; }

cat > "$OUT.lib.pli" <<'EOF'
 efflib: procedure(x) returns(fixed bin(31));
    declare x fixed bin(31);
    return(x * 3 + 1);
 end efflib;
EOF

cat > "$OUT.main.pli" <<'EOF'
 effmain: procedure options(main);
    declare efflib entry(fixed bin(31)) returns(fixed bin(31)) external;
    declare r fixed bin(31);
    r = efflib(21);
    put skip list('effects', r);
 end effmain;
EOF

# --- the callee publishes what it promises ---------------------------------
# The default build has no size checks, so the helper cannot trap: it reads
# its argument, writes nothing, never unwinds and always returns. The line
# carries the digest of the signature the promise was published for.
$PLI "$OUT.lib.pli" --emit-effects="$OUT.eff" -c -o "$OUT.lib.o" ||
  fail "the publishing compile failed"
grep -q '^EFFLIB [0-9a-f]\{16\} argmem-read nounwind willreturn$' "$OUT.eff" ||
  fail "the published file does not carry the promise"

# --- the caller attaches the promise to its declaration --------------------
$PLI "$OUT.main.pli" --effects="$OUT.eff" -emit-llvm -o "$OUT.prom.ll" ||
  fail "the consuming compile failed"
grep -q 'nounwind willreturn memory(argmem: read)' "$OUT.prom.ll" ||
  fail "the promise did not reach the declaration"

# --- without the file the declaration promises nothing ---------------------
$PLI "$OUT.main.pli" -emit-llvm -o "$OUT.plain.ll" || fail "the plain compile failed"
grep -q 'memory(argmem: read)' "$OUT.plain.ll" &&
  fail "the promise appeared without the published file"

# --- a promise published for another signature is not a promise here -------
sed 's/^EFFLIB [0-9a-f]\{16\}/EFFLIB 0000000000000000/' "$OUT.eff" > "$OUT.othersig.eff"
$PLI "$OUT.main.pli" --effects="$OUT.othersig.eff" -emit-llvm -o "$OUT.othersig.ll" ||
  fail "the mismatched-signature compile failed"
grep -q 'memory(argmem: read)' "$OUT.othersig.ll" &&
  fail "a promise published for another signature was used"

# --- a file that is not a published-effects file is refused ----------------
printf 'EFFLIB argmem-read nounwind willreturn\n' > "$OUT.nohdr.eff"
if $PLI "$OUT.main.pli" --effects="$OUT.nohdr.eff" -c -o "$OUT.nohdr.o" 2>"$OUT.nohdr.log"; then
  fail "a file without the header was accepted"
fi
grep -q 'not a published-effects file' "$OUT.nohdr.log" ||
  fail "no diagnostic for a file without the header"

# --- the program still computes the right answer ---------------------------
$PLI "$OUT.main.pli" "$OUT.lib.pli" --effects="$OUT.eff" -o "$OUT.exe" ||
  fail "the two-unit link failed"
out=$("$OUT.exe") || fail "the program did not run"
[ "$out" = "effects 64" ] || fail "the program printed '$out'"

# --- a malformed line is an error, not a silent skip -----------------------
{ head -1 "$OUT.eff"; echo 'BOGUS not-a-flag'; } > "$OUT.bad.eff"
if $PLI "$OUT.main.pli" --effects="$OUT.bad.eff" -c -o "$OUT.bad.o" 2>"$OUT.bad.log"; then
  fail "a malformed effects file was accepted"
fi
grep -q 'malformed effect line' "$OUT.bad.log" ||
  fail "no diagnostic for a malformed effects file"

echo "PASS effects-file"
exit 0
