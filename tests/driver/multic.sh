#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM
# tests/driver/multic.sh — `plic -c a.pli b.pli` writes <base>.o per input in
# the cwd (cc behavior), and rejects -o with multiple inputs. Runs from the
# gitignored out/ dir so the .o artifacts stay out of the repo; links the two
# objects to prove they link cleanly.
set -u
# Windows (MSYS/MinGW/Cygwin sh): linked binaries need a .exe suffix, there
# is no libm (the UCRT provides it), and program output uses CRLF. On Unix
# EXE is empty, MATHLIB stays -lm, and stripping CR is a no-op.
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe; MATHLIB=; RTLIBFLAGS=--rtlib=compiler-rt ;;
  *) EXE=; MATHLIB=-lm; RTLIBFLAGS= ;;
esac
PLIC=${PLIC:-$PWD/build/plic}
CLANG=${CLANG:-clang}
RTLIB=${RTLIB:-$PWD/build/libpli.a}
OUT=tests/driver/out/multic
mkdir -p tests/driver/out
cd tests/driver/out

cat > multic_a.pli <<'EOF'
 aproc: procedure;
    put skip list('PASS multic a');
 end aproc;
EOF
cat > multic_b.pli <<'EOF'
 bmain: procedure options(main);
    declare aproc entry external;
    call aproc;
 end bmain;
EOF

$PLIC -c multic_a.pli multic_b.pli || { echo "FAIL multic: -c multi failed"; exit 1; }
[ -f multic_a.o ] && [ -f multic_b.o ] || { echo "FAIL multic: missing .o outputs"; exit 1; }
# -o with multiple inputs must be rejected (cc: "cannot specify -o ...").
if $PLIC -c multic_a.pli multic_b.pli -o x.o 2>/dev/null; then
  echo "FAIL multic: accepted -o with multiple -c inputs"
  exit 1
fi
# -lm after the archive: libpli needs libm on Linux (no-op on macOS).
$CLANG multic_b.o multic_a.o "$RTLIB" $MATHLIB $RTLIBFLAGS -o multic$EXE || { echo "FAIL multic: link"; exit 1; }
./multic$EXE
exit 0
