#!/bin/sh
# tests/driver/explain.sh — `--explain <rule>` prints the TR 25.084 production.
# Exercises rule (16), a multi-line wrapped rule (128), and the error paths.
set -u
PLIC=./build/plic

ok=1
check() {
  if [ "$2" != "$3" ]; then
    echo "FAIL: --explain $1:"
    echo "  expected: $3"
    echo "  got:      $2"
    ok=0
  fi
}

check 16 "$($PLIC --explain 16)" \
  "(16 ) arithmetic-attribute ::= [ { FLOAT | FIXED } ] [ { DECIMAL | BINARY } ] [ { REAL | COMPLEX } ] [ ( integer [ , signed-integer ] ) ]"
check 128 "$($PLIC --explain 128)" \
  "(128) constant ::= real-constant | imaginary-constant | sterling-constant | simple-string-constant | replicated-string-constant"

$PLIC --explain 0 >/dev/null 2>&1; [ $? -ne 0 ] || { echo "FAIL: --explain 0 not rejected"; ok=0; }
$PLIC --explain abc >/dev/null 2>&1; [ $? -ne 0 ] || { echo "FAIL: --explain abc not rejected"; ok=0; }

[ "$ok" -eq 1 ] && echo PASS
exit 0
