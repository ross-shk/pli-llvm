#!/bin/sh
# tests/run_one.sh — compile, run and classify a single test; write the result
# (PASS/FAIL line plus any diagnostics) to <group>/out/<name>.result.
#
# Invoked once per test by run_tests.sh in parallel (via `xargs -P`), so the
# body must be self-contained. Reads its PLIC/RTLIB/CLANG from the environment
# and the timeout helper from scripts/.
#
# Usage: run_one.sh <dir> <name> <type>
#   dir   tests/<group>/
#   name  test basename without extension
#   type  driver | exec | diag
#        driver — <dir>/<name>.sh runs a plic sub-command
#        exec   — <dir>/<name>.pli compiles+links+runs (may have a .c companion)
#        diag   — <dir>/<name>.pli must be rejected by -fsyntax-only
set -u

dir=$1
name=$2
type=$3
out="$dir/out"
mkdir -p "$out"
result="$out/$name.result"

# Run a command with stdout+stderr to `$1`, killing its process group if it
# exceeds 10s. Exit 124 follows the GNU timeout convention.
timeout_run() {
  python3 scripts/run_with_timeout.py 10 "$@"
}

rc=1
{
  case "$type" in
    driver)
      if timeout_run "$out/$name.out" sh "$dir/$name.sh"; [ $? -eq 124 ]; then
        echo "FAIL $name (timed out after 10s)"; echo
      elif [ -f "$dir/expected/$name.out" ]; then
        if diff -u "$dir/expected/$name.out" "$out/$name.out" > "$out/$name.diff" 2>&1; then
          echo "PASS $name"; rc=0
        else
          echo "FAIL $name (output differs)"; sed 's/^/      /' "$out/$name.diff"
        fi
      elif grep -qi 'PASS' "$out/$name.out" && ! grep -qi 'FAIL' "$out/$name.out"; then
        echo "PASS $name"; rc=0
      else
        echo "FAIL $name (self test did not print PASS)"; echo
        sed 's/^/      /' "$out/$name.out"
      fi
      ;;
    exec)
      src="$dir/$name.pli"
      if [ -f "$dir/$name.c" ]; then
        # Cross-unit test: the .pli calls an external C procedure via ENTRY; a
        # companion .c defines it. Compile each to an object and link with the
        # runtime.
        if ! "$CLANG" -c "$dir/$name.c" -o "$out/$name.c.o" > "$out/$name.compile" 2>&1 \
           || ! "$PLIC" "$src" -c -o "$out/$name.pli.o" >> "$out/$name.compile" 2>&1 \
           || ! "$CLANG" "$out/$name.pli.o" "$out/$name.c.o" "$RTLIB" -o "$out/$name" \
                  >> "$out/$name.compile" 2>&1; then
          echo "FAIL $name (cross-unit build failed)"
          sed 's/^/      /' "$out/$name.compile"
        elif timeout_run "$out/$name.out" "$out/$name"; [ $? -eq 124 ]; then
          echo "FAIL $name (timed out after 10s)"; echo
        elif [ -f "$dir/expected/$name.out" ]; then
          if diff -u "$dir/expected/$name.out" "$out/$name.out" > "$out/$name.diff" 2>&1; then
            echo "PASS $name"; rc=0
          else
            echo "FAIL $name (output differs)"; sed 's/^/      /' "$out/$name.diff"
          fi
        elif grep -qi 'PASS' "$out/$name.out" && ! grep -qi 'FAIL' "$out/$name.out"; then
          echo "PASS $name"; rc=0
        else
          echo "FAIL $name (self test did not print PASS)"; echo
          sed 's/^/      /' "$out/$name.out"
        fi
      elif ! "$PLIC" "$src" -o "$out/$name" > "$out/$name.compile" 2>&1; then
        echo "FAIL $name (compilation failed)"
        sed 's/^/      /' "$out/$name.compile"
      elif timeout_run "$out/$name.out" "$out/$name"; [ $? -eq 124 ]; then
        echo "FAIL $name (timed out after 10s)"; echo
      elif [ -f "$dir/expected/$name.out" ]; then
        # Golden test: diff against the recorded baseline.
        if diff -u "$dir/expected/$name.out" "$out/$name.out" > "$out/$name.diff" 2>&1; then
          echo "PASS $name"; rc=0
        else
          echo "FAIL $name (output differs)"; sed 's/^/      /' "$out/$name.diff"
        fi
      elif grep -qi 'PASS' "$out/$name.out" && ! grep -qi 'FAIL' "$out/$name.out"; then
        # Self-contained test: the program verifies itself.
        echo "PASS $name"; rc=0
      else
        echo "FAIL $name (self test did not print PASS)"; echo
        sed 's/^/      /' "$out/$name.out"
      fi
      ;;
    diag)
      # Diagnostic tests: these must be rejected.
      if "$PLIC" "$dir/$name.pli" -fsyntax-only > "$out/$name.compile" 2>&1; then
        echo "FAIL $name (expected diagnostics, compiled cleanly)"
      else
        echo "PASS $name (rejected as expected)"; rc=0
      fi
      ;;
  esac
} > "$result"

exit "$rc"
