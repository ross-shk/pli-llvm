#!/bin/sh
# tests/run_tests.sh — compile, run and check every test program, in parallel.
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
#
# Tests are dispatched to tests/run_one.sh through `xargs -P`, so independent
# tests compile and run concurrently. `JOBS` overrides the default worker count
# (the number of CPUs). Results are collected and printed in group order.
# `make -j` parallelises the compile step separately (see Makefile).
set -u

cd "$(dirname "$0")/.." || exit 1
PLIC=${PLIC:-./build/plic}
RTLIB=${RTLIB:-./build/libpli.a}
CLANG=${CLANG:-clang}
export PLIC RTLIB CLANG

# Parallelism: JOBS or the number of CPUs (fall back to 4 if unknown).
if [ -z "${JOBS:-}" ]; then
  JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
fi

# The job list: one line per test, `dir name type`, in deterministic order.
jobs=""
append_job() { jobs="$jobs$1 $2 $3
"; }

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
  have_pli=0; [ -f "$1" ] && have_pli=1
  set -- "$dir"*.sh
  have_sh=0; [ -f "$1" ] && have_sh=1
  [ "$have_pli" -eq 1 ] || [ "$have_sh" -eq 1 ] || continue

  # Driver tests: tests/driver/*.sh run a plic sub-command.
  if [ "$have_sh" -eq 1 ]; then
    for drv in "$dir"*.sh; do
      [ -f "$drv" ] || continue
      name=$(basename "$drv" .sh)
      if [ "$single" -eq 1 ] && [ "$name" != "$onetest_name" ]; then continue; fi
      append_job "$dir" "$name" driver
    done
  fi

  # Execution tests: compile, run, then diff or self-check.
  for src in "$dir"*.pli; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .pli)
    if [ "$single" -eq 1 ] && [ "$name" != "$onetest_name" ]; then continue; fi
    case "$name" in
      bad_*) continue ;;
    esac
    append_job "$dir" "$name" exec
  done

  # Diagnostic tests: these must be rejected.
  for src in "$dir"bad_*.pli; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .pli)
    if [ "$single" -eq 1 ] && [ "$name" != "$onetest_name" ]; then continue; fi
    append_job "$dir" "$name" diag
  done
done

# Run all jobs concurrently (bounded by JOBS), then print results in order.
jobsfile=$(mktemp "${TMPDIR:-/tmp}/plic-jobs.XXXXXX")
printf '%s' "$jobs" > "$jobsfile"
cat "$jobsfile" | xargs -P "$JOBS" -n 3 tests/run_one.sh

pass=0
fail=0
while read -r dir name type; do
  result="$dir/out/$name.result"
  if [ -f "$result" ]; then
    cat "$result"
    case "$(sed -n '1s/^PASS.*/PASS/p' "$result")" in
      PASS) pass=$((pass + 1)) ;;
      *) fail=$((fail + 1)) ;;
    esac
  else
    echo "FAIL $name (no result)"
    fail=$((fail + 1))
  fi
done < "$jobsfile"
rm -f "$jobsfile"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
