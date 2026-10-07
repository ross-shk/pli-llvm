#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM
# tests/driver/multiemit.sh — `plic -emit-llvm a.pli b.pli` writes a .ll per
# input (cc-like per-file mode), and rejects -o with multiple inputs. Runs from
# the gitignored out/ dir so the .ll artifacts stay out of the repo.
set -u
PLIC=${PLIC:-$PWD/build/plic}
OUT=tests/driver/out/multiemit
mkdir -p tests/driver/out
cd tests/driver/out

cat > multiemit_a.pli <<'EOF'
 aproc: procedure;
    declare x fixed bin(31) init(1);
    x = x + 1;
 end aproc;
EOF
cat > multiemit_b.pli <<'EOF'
 bmain: procedure options(main);
    declare aproc entry external;
    call aproc;
 end bmain;
EOF

$PLIC -emit-llvm multiemit_a.pli multiemit_b.pli || { echo "FAIL multiemit: -emit-llvm multi failed"; exit 1; }
[ -f multiemit_a.ll ] && [ -f multiemit_b.ll ] || { echo "FAIL multiemit: missing .ll outputs"; exit 1; }
# Each unit's external symbols must be present in its own .ll.
grep -q "@APROC" multiemit_a.ll || { echo "FAIL multiemit: APROC not in a.ll"; exit 1; }
grep -q "declare.*@APROC" multiemit_b.ll || { echo "FAIL multiemit: APROC not declared in b.ll"; exit 1; }
# -o with multiple inputs must be rejected in a per-file mode.
if $PLIC -emit-llvm multiemit_a.pli multiemit_b.pli -o x.ll 2>/dev/null; then
  echo "FAIL multiemit: accepted -o with multiple -emit-llvm inputs"
  exit 1
fi
echo PASS
exit 0
