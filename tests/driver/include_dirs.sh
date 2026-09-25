#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM
# tests/driver/include_dirs.sh — %INCLUDE search paths (ADR-078):
# the including file's directory, repeatable -I (first wins),
# PLIC_INCLUDE_PATH, and the executable-relative default dir; a missing
# file still fails the compile. Runs from the repo root; fixtures live in
# the gitignored out/ dir.
set -u
PLIC=./build/plic
OUT=./tests/driver/out/include_dirs
mkdir -p tests/driver/out/inc_a tests/driver/out/inc_b

cat > tests/driver/out/inc_a/defs.inc <<'EOF'
 magic = 41 + 1;
EOF
cat > tests/driver/out/inc_b/other.inc <<'EOF'
 other = magic + 1;
EOF
cat > tests/driver/out/inc_a/dup.inc <<'EOF'
 dup = 1;
EOF
cat > tests/driver/out/inc_b/dup.inc <<'EOF'
 dup = 2;
EOF
cat > "$OUT.pli" <<'EOF'
 include_dirs: procedure options(main);
    declare magic fixed bin(31);
    declare other fixed bin(31);
    declare dup fixed bin(31);
    declare fails fixed bin(31);
    fails = 0;
    %include 'defs.inc';
    %include 'other.inc';
    %include 'dup.inc';
    if magic ^= 42 then do;
       fails = fails + 1;
       put skip list('FAIL magic');
    end;
    if other ^= 43 then do;
       fails = fails + 1;
       put skip list('FAIL other');
    end;
    if dup ^= 1 then do;
       fails = fails + 1;
       put skip list('FAIL dup order');
    end;
    if fails = 0 then put skip list('PASS include-dirs');
    else put skip list('FAIL include-dirs');
 end include_dirs;
EOF
cat > "$OUT.missing.pli" <<'EOF'
 include_missing: procedure options(main);
    %include 'no_such_file.inc';
 end include_missing;
EOF

# -I finds inc_a; the env path finds inc_b; first -I wins for dup.inc.
PLIC_INCLUDE_PATH=tests/driver/out/inc_b \
  $PLIC -I tests/driver/out/inc_a -I tests/driver/out/inc_b \
  "$OUT.pli" -o "$OUT" || { echo "FAIL include-dirs: compile"; exit 1; }
"$OUT" || { echo "FAIL include-dirs: run"; exit 1; }

# A missing file still fails the compile.
if $PLIC -I tests/driver/out/inc_a "$OUT.missing.pli" -o "$OUT.missing" 2>/dev/null; then
  echo "FAIL include-dirs: missing file compiled";
  exit 1;
fi
echo "PASS include-missing";

# -v lists the executable-relative default dir.
if $PLIC -v -fsyntax-only "$OUT.pli" 2>&1 | grep -q "share/plic/include"; then
  echo "PASS include-default";
else
  echo "FAIL include-dirs: default dir not listed";
  exit 1;
fi
