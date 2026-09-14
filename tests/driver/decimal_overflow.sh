#!/bin/sh
# tests/driver/decimal_overflow.sh — QR1.2 FIXED DECIMAL precision overflow:
# add/mul and narrowing conversions trap with an ERROR (hard error until a
# SIZE condition can route them); subtraction shares the add path
# (checked-ssub plus conversion trap, as in driver/overflow for binary).
# In-range edges, including rescaled narrowing that fits, run clean.
set -u
PLIC=./build/plic
OUT=./tests/driver/out/decimal_overflow
mkdir -p tests/driver/out

gen() {
cat > "$OUT.$1.pli" <<EOF
 decimal_overflow_$1: procedure options(main);
 $2
 $3
 end decimal_overflow_$1;
EOF
}

# Each overflow case must abort (rc != 0) with the overflow message.
expect_abort() {
  $PLIC "$OUT.$1.pli" -o "$OUT.$1" 2>/dev/null || { echo "FAIL decimal-overflow: $1 compile"; exit 1; }
  out=$("$OUT.$1" 2>&1); rc=$?
  case "$out" in
    *overflow*) ;;
    *) echo "FAIL decimal-overflow: $1 wrong message"; exit 1;;
  esac
  if [ "$rc" -eq 0 ]; then echo "FAIL decimal-overflow: $1 exited 0"; exit 1; fi
  echo "PASS decimal-overflow-$1";
}

# Addition assigns back narrower: 999.99 + 0.01 needs 1000.00 > (5,2).
gen add "declare a fixed decimal(5,2); declare b fixed decimal(5,2);" \
  "a = 999.99; b = a + 0.01; put skip list(b);"
# Multiplication wraps the i32 intermediate; a wide target cannot save it.
gen mul "declare a fixed decimal(9,2); declare b fixed decimal(9,2); declare c fixed decimal(15,4);" \
  "a = 9999999.99; b = 9999999.99; c = a * b; put skip list(c);"
# Narrowing conversion past the declared digits: 1000.00 into (5,2).
gen conv "declare a fixed decimal(9,2); declare b fixed decimal(5,2);" \
  "a = 1000.00; b = a; put skip list(b);"
expect_abort add
expect_abort mul
expect_abort conv

# In-range edges run and print.
cat > "$OUT.ok.pli" <<'EOF'
 decimal_overflow_ok: procedure options(main);
    declare a fixed decimal(5,2);
    declare b fixed decimal(9,2);
    declare c fixed decimal(9,4);
    declare d fixed decimal(5,2);
    declare i fixed binary(31);
    a = 999.99;
    b = a + 0.00;
    put skip list(b);
    b = 10.00;
    c = b * 10.00;
    put skip list(c);
    b = 100.00;
    d = b;
    put skip list(d);
    i = d;
    put skip list(i);
 end decimal_overflow_ok;
EOF
$PLIC "$OUT.ok.pli" -o "$OUT.ok" || { echo "FAIL decimal-overflow: ok compile"; exit 1; }
if [ "$("$OUT.ok")" = "999.99
100.0000
100.00
100" ]; then
  echo "PASS decimal-overflow-ok";
else
  echo "FAIL decimal-overflow: ok output"; exit 1;
fi
