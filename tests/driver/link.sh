#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM
# tests/driver/link.sh — the driver's link-step flags (-c, -L/-l, -Wl,
# --linker, -shared, --extra) and a cross-unit link against a C unit + libpli.
set -u
# Windows (MSYS/MinGW/Cygwin sh): linked binaries need a .exe suffix, there
# is no libm (the UCRT provides it), and program output uses CRLF. On Unix
# EXE is empty, MATHLIB stays -lm, and stripping CR is a no-op.
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe; MATHLIB= ;;
  *) EXE=; MATHLIB=-lm ;;
esac
PLIC=${PLIC:-./build/plic}
CLANG=${CLANG:-clang}
RTLIB=${RTLIB:-./build/libpli.a}
OUT=tests/driver/out/link
mkdir -p tests/driver/out

ok=1
check() {
  # Strip CR: Windows programs emit CRLF (a no-op on Unix).
  set -- "$1" "$(printf '%s' "$2" | tr -d '\r')" "$3"
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
# (-lm after the archive: libpli's ROUND/mathport refs need libm on Linux;
# harmless on macOS where libm is in libSystem.)
$CLANG "$OUT.pli.o" "$OUT.c.o" "$RTLIB" $MATHLIB -o "$OUT$EXE" || { echo "FAIL: cross-unit link failed"; ok=0; }
check "cross-unit run" "$($OUT$EXE)" "plic: 42"

# -Wl/--linker/-shared/nm probe Unix link behavior; skip on Windows.
if [ -n "$EXE" ]; then
  echo "SKIP: Unix-only link checks (-Wl, --linker, -shared, nm)"
else
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

# --- dead code: hello-world must not carry unused runtime functions --------
cat > "$OUT.hello.pli" <<'EOF'
 hello: procedure options(main);
    put skip list('hello');
 end hello;
EOF
$PLIC "$OUT.hello.pli" -o "$OUT.hello" || { echo "FAIL: hello link failed"; ok=0; }
nm "$OUT.hello" | grep -q "pli_put_list_char" || { echo "FAIL: used runtime fn stripped"; ok=0; }
nm "$OUT.hello" | grep -q "pli_sin" && { echo "FAIL: unused runtime fn linked (bloat)"; ok=0; }
fi

[ "$ok" -eq 1 ] && echo PASS
exit 0
