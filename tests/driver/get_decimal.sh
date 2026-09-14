#!/bin/sh
# tests/driver/get_decimal.sh — GET LIST into FIXED DECIMAL (QR1.2):
# "12.5" reads 1250 into a dec(4,2); the program verifies itself.
set -u
PLIC=./build/plic
OUT=./tests/driver/out/get_decimal
mkdir -p tests/driver/out

cat > "$OUT.pli" <<'EOF'
 get_decimal: procedure options(main);
    declare d fixed dec(4, 2);
    declare fails fixed bin(31);
    fails = 0;
    get list (d);
    if d ^= 12.5 then do;
       fails = fails + 1;
       put skip list('FAIL d');
    end;
    if fails = 0 then put skip list('PASS get-decimal');
    else put skip list('FAIL get-decimal');
 end get_decimal;
EOF

$PLIC "$OUT.pli" -o "$OUT" || \
  { echo "FAIL get-decimal: compile"; exit 1; }
printf '12.5\n' | "$OUT"
exit 0
