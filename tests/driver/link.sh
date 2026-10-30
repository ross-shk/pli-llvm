#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/link.sh — the driver's link-step flags (-c, -L/-l, -Wl,
# --extra) and a cross-unit link against a C unit.
# All links are in-process lld (ADR-225): the C unit still needs a C compiler
# (CLANG, user-side tool for C sources), but the final link always goes
# through pli itself, never a clang driver.
set -u
# Windows (MSYS/MinGW/Cygwin sh): linked binaries need a .exe suffix, there
# is no libm (the UCRT provides it), and program output uses CRLF. On Unix
# EXE is empty and stripping CR is a no-op.
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
  *) EXE= ;;
esac
PLI=${PLI:-./build/cmake/pli}
CLANG=${CLANG:-clang}
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

# --- cross-unit: pli -c object linked with a C unit and pli_runtime -------------
cat > "$OUT.pli" <<'EOF'
 hello: procedure options(main);
    declare x fixed binary(31) init(21);
    declare double_it entry (fixed binary(31))
       external('double_it');
    call double_it(x);
    put skip list('pli:', x);
 end hello;
EOF
cat > "$OUT.c" <<'EOF'
void double_it(int *x) { *x = 2 * *x; }
EOF

$PLI "$OUT.pli" -c -o "$OUT.pli.o" || { echo "FAIL: pli -c failed"; ok=0; }
$CLANG -c "$OUT.c" -o "$OUT.c.o" || { echo "FAIL: clang -c failed"; ok=0; }
# Link pli object and C object into one binary with pli itself (in-process
# lld; the -c MAIN object already carries the merged runtime definitions, so
# no runtime archive is needed). pli adds -lm on ELF itself.
$PLI "$OUT.pli.o" "$OUT.c.o" -o "$OUT$EXE" || { echo "FAIL: cross-unit link failed"; ok=0; }
check "cross-unit run" "$($OUT$EXE)" "pli: 42"

# -Wl/nm probe Unix link behavior; skip on Windows.
if [ -n "$EXE" ]; then
  echo "SKIP: Unix-only link checks (-Wl, nm)"
else
# --- -Wl reaches in-process lld (via -v), in platform spelling ------------
# (ELF lld wants -no-pie with a hyphen; Mach-O wants -no_pie).
# Uses the self-contained hello fixture below (link.pli needs its C unit).
cat > "$OUT.flag.pli" <<'EOF'
 hello_flag: procedure options(main);
    put skip list('hello');
 end hello_flag;
EOF
cmd=$($PLI "$OUT.flag.pli" -o "$OUT.wl" -Wl,-no_pie -v 2>&1) \
  || { echo "FAIL: -Wl build failed"; ok=0; }
case "$(uname -s)" in
  Darwin) want=-no_pie ;;
  *) want=-no-pie ;;
esac
printf '%s\n' "$cmd" | grep -q -- "$want" || { echo "FAIL: -Wl flag not passed through"; ok=0; }

# --- dead code: hello-world must not carry unused runtime functions --------
cat > "$OUT.hello.pli" <<'EOF'
 hello: procedure options(main);
    put skip list('hello');
 end hello;
EOF
$PLI "$OUT.hello.pli" -o "$OUT.hello" || { echo "FAIL: hello link failed"; ok=0; }
nm "$OUT.hello" | grep -q "pli_put_list_char" || { echo "FAIL: used runtime fn stripped"; ok=0; }
nm "$OUT.hello" | grep -q "pli_sin" && { echo "FAIL: unused runtime fn linked (bloat)"; ok=0; }
fi

[ "$ok" -eq 1 ] && echo PASS
exit 0
