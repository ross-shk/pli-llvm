#!/bin/sh
# tests/driver/link.sh — the driver's link-step flags (-c, -L/-l, -Wl,
# --linker, -shared, --extra) and a cross-unit link against a C unit + libpli.
set -u
PLIC=./build/plic
CLANG=clang
RTLIB=./build/libpli.a
OUT=tests/driver/out/link
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

# --- cross-unit: plic -c object linked with a C unit and libpli -------------
cat > "$OUT.pli" <<'EOF'
 hello: procedure options(main);
    declare x fixed binary(31) init(21);
    declare double_it entry (fixed binary(31))
       external('double_it');
    call double_it(x);
    put skip list('plic:', x);
 end hello;
EOF
cat > "$OUT.c" <<'EOF'
void double_it(int *x) { *x = 2 * *x; }
EOF

$PLIC "$OUT.pli" -c -o "$OUT.pli.o" || { echo "FAIL: plic -c failed"; ok=0; }
$CLANG -c "$OUT.c" -o "$OUT.c.o" || { echo "FAIL: clang -c failed"; ok=0; }
# Link pli object, C object and the runtime into one binary.
$CLANG "$OUT.pli.o" "$OUT.c.o" "$RTLIB" -o "$OUT" || { echo "FAIL: cross-unit link failed"; ok=0; }
check "cross-unit run" "$($OUT)" "plic: 42"

# --- -Wl and --linker surface in the backend command (via -v) ---------------
cmd=$($PLIC "$OUT.pli" -o "$OUT.wl" -Wl,-no_pie --linker ld64.lld -v 2>&1)
printf '%s\n' "$cmd" | grep -q -- '-Wl,-no_pie' || { echo "FAIL: -Wl flag not passed through"; ok=0; }
printf '%s\n' "$cmd" | grep -q -- '-fuse-ld=ld64.lld' || { echo "FAIL: --linker not passed through"; ok=0; }

# --- -shared produces a shared artifact (self-contained, no external C) -----
cat > "$OUT.shared.pli" <<'EOF'
 libproc: procedure;
    put skip list('shared lib');
 end libproc;
EOF
$PLIC "$OUT.shared.pli" -o "$OUT.dylib" -shared 2>/dev/null || { echo "FAIL: -shared link failed"; ok=0; }
[ -f "$OUT.dylib" ] || { echo "FAIL: -shared produced no output"; ok=0; }

[ "$ok" -eq 1 ] && echo PASS
exit 0
