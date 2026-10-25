#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/undefinedfile.sh — UNDEFINEDFILE (rule (97)): a failing OPEN
# raises the condition against the FILE variable. With no handler, or after a
# REVERT, the implicit action is ERROR and the program aborts. A handler runs
# and resumes after the OPEN, and the file stays closed: a later PUT or GET on
# it raises ERROR instead of silently falling back to SYSPRINT or SYSIN.
set -u
# Windows (MSYS/MinGW/Cygwin sh): linked binaries need a .exe suffix.
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
  *) EXE= ;;
esac
PLI=${PLI:-./build/cmake/pli}
OUT=./tests/driver/out/undefinedfile
mkdir -p tests/driver/out

# expect_abort <case> <message fragment> [<text that must not appear>]
expect_abort() {
  $PLI "$OUT.$1.pli" -o "$OUT.$1$EXE" 2>/dev/null || { echo "FAIL undefinedfile: $1 compile"; exit 1; }
  out=$("$OUT.$1$EXE" 2>&1); rc=$?
  case "$out" in
    *"$2"*) ;;
    *) echo "FAIL undefinedfile: $1 wrong message"; exit 1;;
  esac
  if [ $# -ge 3 ]; then
    case "$out" in
      *"$3"*) echo "FAIL undefinedfile: $1 leaked '$3'"; exit 1;;
    esac
  fi
  if [ "$rc" -eq 0 ]; then echo "FAIL undefinedfile: $1 exited 0"; exit 1; fi
  echo "PASS undefinedfile-$1";
}

# expect_ok <case> <expected output>
expect_ok() {
  $PLI "$OUT.$1.pli" -o "$OUT.$1$EXE" 2>/dev/null || { echo "FAIL undefinedfile: $1 compile"; exit 1; }
  got=$("$OUT.$1$EXE" | tr -d '\r'); rc=$?
  if [ "$rc" -ne 0 ]; then echo "FAIL undefinedfile: $1 exited $rc"; exit 1; fi
  if [ "$got" != "$2" ]; then
    echo "FAIL undefinedfile: $1 output was '$got'"
    exit 1
  fi
  echo "PASS undefinedfile-$1";
}

# No handler: the OPEN failure aborts with the OPEN message.
cat > "$OUT.default.pli" <<'EOF'
 undefinedfile_default: procedure options(main);
    declare f file;
    open file(f) input title('tests/driver/out/undefinedfile_absent.dat');
    put skip list('not reached');
 end undefinedfile_default;
EOF
expect_abort default "OPEN failed"

# A REVERT before the OPEN drops the unit: the implicit ERROR action aborts,
# and the unit must not run.
cat > "$OUT.revert.pli" <<'EOF'
 undefinedfile_revert: procedure options(main);
    declare f file;
    on undefinedfile(f) put skip list('caught');
    revert undefinedfile(f);
    open file(f) input title('tests/driver/out/undefinedfile_absent.dat');
    put skip list('not reached');
 end undefinedfile_revert;
EOF
expect_abort revert "OPEN failed" "caught"

# A handler resumes after the OPEN and stays established for the file.
cat > "$OUT.resume.pli" <<'EOF'
 undefinedfile_resume: procedure options(main);
    declare f file;
    on undefinedfile(f) put skip list('handled');
    open file(f) input title('tests/driver/out/undefinedfile_absent.dat');
    put skip list('after');
 end undefinedfile_resume;
EOF
expect_ok resume "handled
after"

# The file stays closed: a PUT on it aborts instead of writing to the console.
cat > "$OUT.put.pli" <<'EOF'
 undefinedfile_put: procedure options(main);
    declare f file;
    on undefinedfile(f) put skip list('handled');
    open file(f) output title('tests/driver/out/no_such_dir/data.dat');
    put file(f) list('leaked');
    put skip list('not reached');
 end undefinedfile_put;
EOF
expect_abort put "not open" "leaked"

# ... and a GET aborts instead of reading SYSIN.
cat > "$OUT.get.pli" <<'EOF'
 undefinedfile_get: procedure options(main);
    declare f file;
    declare x fixed bin(31);
    on undefinedfile(f) put skip list('handled');
    open file(f) input title('tests/driver/out/undefinedfile_absent.dat');
    get file(f) list(x);
    put skip list('not reached', x);
 end undefinedfile_get;
EOF
expect_abort get "not open" "not reached"

# A RECORD open raises the same condition.
cat > "$OUT.record.pli" <<'EOF'
 undefinedfile_record: procedure options(main);
    declare f file;
    on undefinedfile(f) put skip list('handled');
    open file(f) record sequential input
       title('tests/driver/out/undefinedfile_absent.dat');
    put skip list('after');
 end undefinedfile_record;
EOF
expect_ok record "handled
after"
