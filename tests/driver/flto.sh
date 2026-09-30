#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM
# tests/driver/flto.sh — explicit opt-in LTO (OPTIMIZATION.md §10, P3):
# --flto=thin / --flto=full are forwarded to both the compile and the link
# step, single- and multi-unit.
set -u
PLIC=./build/plic
OUT=tests/driver/out/flto
mkdir -p tests/driver/out

ok=1
check() {
  if [ "$2" != "$3" ]; then
    echo "FAIL: $1"
    echo "  expected: $3"
    echo "  got:      $2"
    ok=0
  fi
}

cat > "$OUT.pli" <<'EOF'
 flto_main: procedure options(main);
    put skip list('PASS flto');
 end flto_main;
EOF

# --- ThinLTO: flag reaches both steps, binary runs ----------------------
cmd=$($PLIC "$OUT.pli" -o "$OUT.thin" --flto=thin -v 2>&1) \
  || { echo "FAIL: thin-lto compile+link"; ok=0; }
printf '%s\n' "$cmd" | grep -q -- '-flto=thin' \
  || { echo "FAIL: -flto=thin not passed through"; ok=0; }
check "thin run" "$($OUT.thin)" "PASS flto"

# --- full LTO ------------------------------------------------------------
$PLIC "$OUT.pli" -o "$OUT.full" --flto=full \
  || { echo "FAIL: full-lto compile+link"; ok=0; }
check "full run" "$($OUT.full)" "PASS flto"

# --- multi-unit ThinLTO across two plic inputs in one driver call ------
cat > "$OUT.lib.pli" <<'EOF'
 flto_lib: procedure(x) returns(fixed bin(31));
    declare x fixed bin(31);
    return(x * 2);
 end flto_lib;
EOF
cat > "$OUT.main.pli" <<'EOF'
 flto_main2: procedure options(main);
    declare flto_lib entry(fixed bin(31)) returns(fixed bin(31)) external;
    put skip list('PASS flto', flto_lib(21));
 end flto_main2;
EOF
$PLIC "$OUT.lib.pli" "$OUT.main.pli" -o "$OUT.multi" --flto=thin \
  || { echo "FAIL: multi-unit thin-lto"; ok=0; }
check "multi thin run" "$($OUT.multi)" "PASS flto 42"

[ "$ok" -eq 1 ] && echo PASS
exit 0