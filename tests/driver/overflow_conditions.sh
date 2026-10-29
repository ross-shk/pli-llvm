#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/overflow_conditions.sh — the implicit actions of OVERFLOW
# (ADR-333) and FIXEDOVERFLOW (ADR-334). Both print a message and raise
# ERROR when no unit is established, so the program aborts; an OVERFLOW
# unit that returns normally raises ERROR while a FIXEDOVERFLOW unit
# resumes, and a unit plus an ERROR unit continues the program.
set -u
# Windows (MSYS/MinGW/Cygwin sh): linked binaries need a .exe suffix and
# program output uses CRLF; on Unix EXE is empty and CR stripping is a no-op.
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
  *) EXE= ;;
esac
PLI=${PLI:-./build/cmake/pli}
OUT=./tests/driver/out/overflow_conditions
mkdir -p "$OUT"

gen() {
cat > "$OUT.$1.pli" <<EOF
 overflow_conditions_$1: procedure options(main);
 $2
 $3
 end overflow_conditions_$1;
EOF
}

# The program must abort with the condition's message in its output.
expect_abort() {
  want=$2
  $PLI --size-checks "$OUT.$1.pli" -o "$OUT.$1$EXE" 2>/dev/null || {
    echo "FAIL overflow-conditions: $1 compile"; exit 1; }
  "$OUT.$1$EXE" > "$OUT.$1.log" 2>&1; rc=$?
  out=$(tr -d '\r' < "$OUT.$1.log")
  case "$out" in
    *"$want"*) ;;
    *) echo "FAIL overflow-conditions: $1 wrong message"; exit 1;;
  esac
  if [ "$rc" -eq 0 ]; then echo "FAIL overflow-conditions: $1 exited 0"; exit 1; fi
  echo "PASS overflow-conditions-$1"
}

# The program must run to completion and print the expected line.
expect_ok() {
  want=$2
  $PLI --size-checks "$OUT.$1.pli" -o "$OUT.$1$EXE" 2>/dev/null || {
    echo "FAIL overflow-conditions: $1 compile"; exit 1; }
  "$OUT.$1$EXE" > "$OUT.$1.log" 2>&1; rc=$?
  out=$(tr -d '\r' < "$OUT.$1.log")
  if [ "$rc" -ne 0 ]; then
    echo "FAIL overflow-conditions: $1 exited $rc"; exit 1
  fi
  case "$out" in
    *"$want"*) ;;
    *) echo "FAIL overflow-conditions: $1 missing '$want'"; exit 1;;
  esac
  echo "PASS overflow-conditions-$1"
}

# OVERFLOW with no unit: message then ERROR aborts.
gen ovf_none "declare big float bin(53); declare x float bin(53);" \
  "big = 1e308; x = big * 10.0; put skip list('after', x);"
expect_abort ovf_none "OVERFLOW: floating-point result too large"

# OVERFLOW unit with no ERROR unit: the normal return raises ERROR.
gen ovf_unit "declare big float bin(53); declare x float bin(53);
 on overflow put skip list('unit');" \
  "big = 1e308; x = big * 10.0; put skip list('after', x);"
expect_abort ovf_unit "normal return from an OVERFLOW unit"

# OVERFLOW unit plus ERROR unit: the unit returns, ERROR runs, and the
# program resumes after the ERROR raise point.
gen ovf_both "declare big float bin(53); declare x float bin(53);
 on overflow put skip list('unit');
 on error begin; put skip list('error-unit'); end;" \
  "big = 1e308; x = big * 10.0; put skip list('resumed');"
expect_ok ovf_both "resumed"

# FIXEDOVERFLOW with no unit: message then ERROR aborts.
gen fof_none "declare a fixed dec(20,0); declare c fixed dec(31,0);" \
  "a = 1000000000000000; c = a * 10000000000000000; put skip list(c);"
expect_abort fof_none "FIXEDOVERFLOW: decimal result longer than 31 digits"

# FIXEDOVERFLOW unit returns normally: execution resumes.
gen fof_unit "declare a fixed dec(20,0); declare c fixed dec(31,0);
 declare n fixed bin(31) init(0);
 on fixedoverflow n = n + 1;" \
  "a = 1000000000000000; c = a * 10000000000000000;
 put skip list('hits', n);"
expect_ok fof_unit "hits 1"

# (nooverflow) lets an infinite result stand and the program finishes.
gen ovf_prefix "declare big float bin(53); declare x float bin(53);" \
  "(nooverflow): begin; big = 1e308; x = big * 10.0; end;
 put skip list('survived');"
expect_ok ovf_prefix "survived"
