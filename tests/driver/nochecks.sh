#!/bin/sh
# tests/driver/nochecks.sh — (NOSUBSCRIPTRANGE) and (NOZERODIVIDE)
# elide their traps (rules (60)-(63), ADR-112): the checks vanish
# from the IR, and formerly-aborting programs complete.
set -u
PLIC=./build/plic
OUT=tests/driver/out/nochecks
mkdir -p tests/driver/out

cat > "$OUT.sub.pli" <<'EOF'
subdemo: procedure options(main);
   declare a(3) fixed bin(31) init(11, 22, 33);
   declare i fixed bin(31);
   declare v fixed bin(31);
   get list(i);
   (nosubscriptrange):
      v = a(i);
   put skip list(v);
end subdemo;
EOF
$PLIC "$OUT.sub.pli" -emit-llvm -o "$OUT.sub.ll" || { echo "FAIL nochecks: sub emit"; exit 1; }
if grep -qE "sub\.fail|pli_subscript_oob" "$OUT.sub.ll"; then echo "FAIL nochecks: subscript checks remain"; exit 1; fi
$PLIC "$OUT.sub.pli" -o "$OUT.sub" || { echo "FAIL nochecks: sub compile"; exit 1; }
echo 9 | "$OUT.sub" >/dev/null 2>&1; rc=$?
if [ "$rc" -ne 0 ]; then echo "FAIL nochecks: prefixed OOB aborted"; exit 1; fi
echo 2 | "$OUT.sub" | grep -q 22 || { echo "FAIL nochecks: in-bounds wrong"; exit 1; }
echo "PASS nochecks-subscriptrange"

cat > "$OUT.zd.pli" <<'EOF'
zddemo: procedure options(main);
   declare z fixed bin(31);
   declare q float;
   get list(z);
   (nozerodivide):
      q = 7 / z;
   put skip list('done');
end zddemo;
EOF
$PLIC "$OUT.zd.pli" -emit-llvm -o "$OUT.zd.ll" || { echo "FAIL nochecks: zd emit"; exit 1; }
if grep -qE "zd\.trap|pli_zerodivide" "$OUT.zd.ll"; then echo "FAIL nochecks: zerodivide checks remain"; exit 1; fi
$PLIC "$OUT.zd.pli" -o "$OUT.zd" || { echo "FAIL nochecks: zd compile"; exit 1; }
echo 0 | "$OUT.zd" >/dev/null 2>&1; rc=$?
if [ "$rc" -ne 0 ]; then echo "FAIL nochecks: prefixed 7/0 aborted"; exit 1; fi
echo 2 | "$OUT.zd" | grep -q done || { echo "FAIL nochecks: nonzero divide wrong"; exit 1; }
echo "PASS nochecks-zerodivide"
exit 0
