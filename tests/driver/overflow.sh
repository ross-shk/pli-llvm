#!/bin/sh
# tests/driver/overflow.sh — QR1.2 FIXED BINARY overflow checks: +-*
# and unary minus abort with an ERROR (hard error until a SIZE condition
# can route them); in-range edge values run clean.
set -u
PLIC=./build/plic
OUT=./tests/driver/out/overflow
mkdir -p tests/driver/out

mk() {
cat > "$OUT.$1.pli" <<EOF
 overflow_$1: procedure options(main);
    declare x fixed bin(31);
    declare y fixed bin(31);
    x = $2;
    y = $3;
    put skip list(y);
 end overflow_$1;
EOF
}

# Each overflow case must abort (rc != 0) with the overflow message.
expect_abort() {
  $PLIC "$OUT.$1.pli" -o "$OUT.$1" 2>/dev/null || { echo "FAIL overflow: $1 compile"; exit 1; }
  out=$("$OUT.$1" 2>&1); rc=$?
  case "$out" in
    *overflow*) ;;
    *) echo "FAIL overflow: $1 wrong message"; exit 1;;
  esac
  if [ "$rc" -eq 0 ]; then echo "FAIL overflow: $1 exited 0"; exit 1; fi
  echo "PASS overflow-$1";
}

mk add 2147483647 "x + 1"
mk sub "-2147483647 - 1" "x - 1"
mk mul 1073741824 "x * 2"
mk neg "-2147483647 - 1" "-x"
expect_abort add
expect_abort sub
expect_abort mul
expect_abort neg

# In-range edges run and print.
cat > "$OUT.ok.pli" <<'EOF'
 overflow_ok: procedure options(main);
    declare x fixed bin(31);
    declare y fixed bin(31);
    x = 2147483647;
    y = x + 0;
    put skip list(y);
    y = x - 2147483647;
    put skip list(y);
 end overflow_ok;
EOF
$PLIC "$OUT.ok.pli" -o "$OUT.ok" || { echo "FAIL overflow: ok compile"; exit 1; }
if [ "$("$OUT.ok")" = "2147483647
0" ]; then
  echo "PASS overflow-ok";
else
  echo "FAIL overflow: ok output";
  exit 1;
fi
