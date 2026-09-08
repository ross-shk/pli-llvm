#!/bin/sh
# tests/run_tests.sh — compile, run and check every test program.
#
# Usage: tests/run_tests.sh [group ... | <group>/<name>.pli ...]
# Without arguments every tests/*/ group runs; otherwise only the named
# groups, or the single tests named by path (e.g. usecases/data.pli).
#
# Each tests/<group>/ subfolder is a test group containing:
#   <group>/*.pli           test programs (bad_*.pli must be rejected)
#   <group>/expected/*.out  recorded stdout for golden tests
#   <group>/out/            scratch binaries, logs and diffs (gitignored)
# A group may mix test classes; each test is classified individually:
#   golden — expected/<name>.out exists: stdout is diff-checked against it
#   self   — no expected/<name>.out: the program verifies itself and must
#            print PASS (case-insensitive); any FAIL in its output fails it
set -u

cd "$(dirname "$0")/.." || exit 1
PLIC=./build/plic
RTLIB=./build/libpli.a

pass=0
fail=0

# Arguments are group names or single-test paths; both must exist. Captured
# before the main loop reuses "$@" for glob expansion.
groups=""
onetest_dir=""
onetest_name=""
for arg in "$@"; do
  case "$arg" in
    *.pli)
      t=${arg#./}
      case "$t" in
        tests/*) ;;
        *) t="tests/$t" ;;
      esac
      [ -f "$t" ] || { echo "run_tests.sh: no such test: $arg"; exit 1; }
      onetest_dir=$(dirname "$t")/
      onetest_name=$(basename "$t" .pli)
      ;;
    *)
      [ -d "tests/$arg" ] || { echo "run_tests.sh: no such test group: tests/$arg"; exit 1; }
      groups="$groups $arg"
      ;;
  esac
done

for dir in tests/*/; do
  # A named group runs whole; with no group arguments, a single-test path
  # selects only its own group, filtered by name inside the loops.
  base=$(basename "$dir")
  named=0
  case " $groups " in
    *" $base "*) named=1 ;;
  esac
  if [ "$named" -eq 1 ]; then
    single=0
  elif [ -n "$onetest_dir" ] && [ "$dir" = "$onetest_dir" ]; then
    single=1
  elif [ -n "$groups" ]; then
    continue
  elif [ -z "$onetest_dir" ]; then
    single=0
  else
    continue
  fi
  set -- "$dir"*.pli
  [ -f "$1" ] || continue
  out="$dir/out"
  mkdir -p "$out"

  # Execution tests: compile, run, then diff or self-check.
  for src in "$dir"*.pli; do
    name=$(basename "$src" .pli)
    if [ "$single" -eq 1 ] && [ "$name" != "$onetest_name" ]; then continue; fi
    case "$name" in
      bad_*) continue ;;
    esac

    if [ -f "$dir/$name.c" ]; then
      # Cross-unit test: the .pli calls an external C procedure via ENTRY;
      # a companion .c defines it. Compile each to an object and link with
      # the runtime.
      if ! clang -c "$dir/$name.c" -o "$out/$name.c.o" > "$out/$name.compile" 2>&1 \
         || ! "$PLIC" "$src" -c -o "$out/$name.pli.o" >> "$out/$name.compile" 2>&1 \
         || ! clang "$out/$name.pli.o" "$out/$name.c.o" "$RTLIB" -o "$out/$name" \
                >> "$out/$name.compile" 2>&1; then
        echo "FAIL $name (cross-unit build failed)"
        sed 's/^/      /' "$out/$name.compile"
        fail=$((fail + 1))
        continue
      fi
    elif ! "$PLIC" "$src" -o "$out/$name" > "$out/$name.compile" 2>&1; then
      echo "FAIL $name (compilation failed)"
      sed 's/^/      /' "$out/$name.compile"
      fail=$((fail + 1))
      continue
    fi

    "$out/$name" > "$out/$name.out" 2>&1
    if [ -f "$dir/expected/$name.out" ]; then
      # Golden test: diff against the recorded baseline.
      if diff -u "$dir/expected/$name.out" "$out/$name.out" > "$out/$name.diff" 2>&1; then
        echo "PASS $name"
        pass=$((pass + 1))
      else
        echo "FAIL $name (output differs)"
        sed 's/^/      /' "$out/$name.diff"
        fail=$((fail + 1))
      fi
    else
      # Self-contained test: the program verifies itself.
      if grep -qi 'PASS' "$out/$name.out" && ! grep -qi 'FAIL' "$out/$name.out"; then
        echo "PASS $name"
        pass=$((pass + 1))
      else
        echo "FAIL $name (self test did not print PASS)"; echo
        sed 's/^/      /' "$out/$name.out"
        fail=$((fail + 1))
      fi
    fi
  done

  # Diagnostic tests: these must be rejected.
  for src in "$dir"bad_*.pli; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .pli)
    if [ "$single" -eq 1 ] && [ "$name" != "$onetest_name" ]; then continue; fi
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
