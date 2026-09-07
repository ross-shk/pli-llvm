#!/bin/sh
# tests/run_tests.sh — compile, run and diff every test program.
#
# Execution tests:  tests/<name>.pli  vs  tests/expected/<name>.out
# Diagnostic tests: tests/bad_*.pli must be rejected (non-zero exit).
set -u

cd "$(dirname "$0")/.." || exit 1
PLIC=./build/plic
OUT=tests/out
mkdir -p "$OUT"

pass=0
fail=0

for src in tests/*.pli; do
  name=$(basename "$src" .pli)
  case "$name" in
    bad_*) continue ;;
  esac

  if ! "$PLIC" "$src" -o "$OUT/$name" > "$OUT/$name.compile" 2>&1; then
    echo "FAIL $name (compilation failed)"
    sed 's/^/      /' "$OUT/$name.compile"
    fail=$((fail + 1))
    continue
  fi

  "$OUT/$name" > "$OUT/$name.out" 2>&1
  if [ ! -f "tests/expected/$name.out" ]; then
    echo "FAIL $name (no expected output; run: $OUT/$name > tests/expected/$name.out)"
    fail=$((fail + 1))
  elif diff -u "tests/expected/$name.out" "$OUT/$name.out" > "$OUT/$name.diff" 2>&1; then
    echo "PASS $name"
    pass=$((pass + 1))
  else
    echo "FAIL $name (output differs)"
    sed 's/^/      /' "$OUT/$name.diff"
    fail=$((fail + 1))
  fi
done

# Diagnostic tests: these must be rejected.
for src in tests/bad_*.pli; do
  [ -f "$src" ] || continue
  name=$(basename "$src" .pli)
  if "$PLIC" "$src" -fsyntax-only > "$OUT/$name.compile" 2>&1; then
    echo "FAIL $name (expected diagnostics, compiled cleanly)"
    fail=$((fail + 1))
  else
    echo "PASS $name (rejected as expected)"
    pass=$((pass + 1))
  fi
done

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
