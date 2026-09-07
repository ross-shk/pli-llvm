#!/bin/sh
# tests/run_tests.sh — compile, run and diff every test program.
#
# Each tests/<group>/ subfolder is a test group containing:
#   <group>/*.pli           test programs (bad_*.pli must be rejected)
#   <group>/expected/*.out  expected stdout, diff-checked
#   <group>/out/            scratch binaries, logs and diffs (gitignored)
set -u

cd "$(dirname "$0")/.." || exit 1
PLIC=./build/plic

pass=0
fail=0

for dir in tests/*/; do
  set -- "$dir"*.pli
  [ -f "$1" ] || continue
  out="$dir/out"
  mkdir -p "$out"

  # Execution tests: compile, run, diff against expected output.
  for src in "$dir"*.pli; do
    name=$(basename "$src" .pli)
    case "$name" in
      bad_*) continue ;;
    esac

    if ! "$PLIC" "$src" -o "$out/$name" > "$out/$name.compile" 2>&1; then
      echo "FAIL $name (compilation failed)"
      sed 's/^/      /' "$out/$name.compile"
      fail=$((fail + 1))
      continue
    fi

    "$out/$name" > "$out/$name.out" 2>&1
    if [ ! -f "$dir/expected/$name.out" ]; then
      echo "FAIL $name (no expected output; run: $out/$name > $dir/expected/$name.out)"
      fail=$((fail + 1))
    elif diff -u "$dir/expected/$name.out" "$out/$name.out" > "$out/$name.diff" 2>&1; then
      echo "PASS $name"
      pass=$((pass + 1))
    else
      echo "FAIL $name (output differs)"
      sed 's/^/      /' "$out/$name.diff"
      fail=$((fail + 1))
    fi
  done

  # Diagnostic tests: these must be rejected.
  for src in "$dir"bad_*.pli; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .pli)
    if "$PLIC" "$src" -fsyntax-only > "$out/$name.compile" 2>&1; then
      echo "FAIL $name (expected diagnostics, compiled cleanly)"
      fail=$((fail + 1))
    else
      echo "PASS $name (rejected as expected)"
      pass=$((pass + 1))
    fi
  done
done

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
