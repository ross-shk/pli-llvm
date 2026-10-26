#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/area_condition.sh — ADR-323: with no ON AREA unit the AREA
# condition takes its implicit action (a message, then ERROR, exit 8). With a
# unit established the same overflow resumes, the ALLOCATE is undone and the
# program carries on.
set -u
# Windows (MSYS/MinGW/Cygwin sh): linked binaries need a .exe suffix.
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
  *) EXE= ;;
esac
PLI=${PLI:-./build/cmake/pli}
OUT=./tests/driver/out/area_condition
mkdir -p tests/driver/out

# The region cannot hold 400 bytes and nothing handles AREA: zero exit is a bug.
cat > "$OUT.unhandled.pli" <<'EOF'
 a_unhandled: procedure options(main);
    declare region area(64);
    declare 1 big based(q),
            2 words(100) fixed bin(31);
    declare q pointer;
    allocate big in(region) set(q);
    put skip list('unreachable');
 end a_unhandled;
EOF
$PLI "$OUT.unhandled.pli" -o "$OUT.unhandled$EXE" 2>/dev/null || {
  echo "FAIL area-condition: unhandled compile"; exit 1; }
out=$("$OUT.unhandled$EXE" 2>&1); rc=$?
case "$out" in
  *"AREA: area overflow"*) ;;
  *) echo "FAIL area-condition: wrong message: $out"; exit 1;;
esac
if [ "$rc" -eq 0 ]; then echo "FAIL area-condition: exited 0"; exit 1; fi
if [ "$rc" -ne 8 ]; then echo "FAIL area-condition: exit $rc"; exit 1; fi

# The same overflow with a unit: the unit runs, the locator stays null, and
# the program reaches its PASS line.
cat > "$OUT.resume.pli" <<'EOF'
 a_resume: procedure options(main);
    declare region area(64);
    declare 1 big based(q),
            2 words(100) fixed bin(31);
    declare q pointer;
    declare hits fixed bin(31) init(0);
    on area hits = hits + 1;
    allocate big in(region) set(q);
    if hits ^= 1 then do;
       put skip list('FAIL area-condition: hits', hits);
       stop;
    end;
    if q ^= null() then do;
       put skip list('FAIL area-condition: locator set');
       stop;
    end;
    put skip list('PASS area-condition');
 end a_resume;
EOF
$PLI "$OUT.resume.pli" -o "$OUT.resume$EXE" 2>/dev/null || {
  echo "FAIL area-condition: resume compile"; exit 1; }
out=$("$OUT.resume$EXE" 2>&1); rc=$?
if [ "$rc" -ne 0 ]; then echo "FAIL area-condition: resume exit $rc: $out"; exit 1; fi
case "$out" in
  *"PASS area-condition"*) ;;
  *) echo "FAIL area-condition: resume output: $out"; exit 1;;
esac

echo "PASS area-condition"
