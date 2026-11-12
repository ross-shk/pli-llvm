#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/multic.sh — `pli -c a.pli b.pli` writes <base>.o per input in
# the cwd (cc behavior), and rejects -o with multiple inputs. Runs from the
# gitignored out/ dir so the .o artifacts stay out of the repo; links the two
# objects to prove they link cleanly.
set -u
PLI=${PLI:-$PWD/build/cmake/pli}
CLANG=${CLANG:-clang}
CC=${CC:-$CLANG}
# Windows (MSYS/MinGW/Cygwin sh): linked binaries need a .exe suffix, there
# is no libm (the UCRT provides it), and program output uses CRLF. On Unix
# EXE is empty, MATHLIB stays -lm, and stripping CR is a no-op.
case "$(uname -s)" in
  # The hermetic stash keeps compiler-rt builtins under lib/windows, outside
  # clang's resource dir, so --rtlib=compiler-rt cannot find them: pass the
  # file ctest resolved, and keep the flag only when CMake did not resolve one.
  # A MinGW-targeting clang is the exception: its driver uses libgcc, which
  # the MinGW sysroot always ships, so --rtlib=compiler-rt there would ask
  # for a libclang_rt.builtins the toolchain does not have.
  MINGW*|MSYS*|CYGWIN*)
    EXE=.exe; MATHLIB=
    case "$($CC -dumpmachine 2>/dev/null)" in
      *-gnu*|*mingw*) RTLIBFLAGS= ;;
      *) RTLIBFLAGS=${CLANG_RT_BUILTINS:---rtlib=compiler-rt} ;;
    esac
    # A PL/I object carries the merged runtime and is built against the
    # project's own MinGW sysroot (platform/windows-mingw), so it must be
    # linked by the driver that owns that sysroot - the same rule link.sh
    # states. A foreign MinGW would supply a different CRT and the result
    # would not run.
    LINKER=$PLI
    ;;
  *) EXE=; MATHLIB=-lm; RTLIBFLAGS=; LINKER=$CC ;;
esac
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

$PLI -c multic_a.pli multic_b.pli || { echo "FAIL multic: -c multi failed"; exit 1; }
[ -f multic_a.o ] && [ -f multic_b.o ] || { echo "FAIL multic: missing .o outputs"; exit 1; }
# -o with multiple inputs must be rejected (cc: "cannot specify -o ...").
if $PLI -c multic_a.pli multic_b.pli -o x.o 2>/dev/null; then
  echo "FAIL multic: accepted -o with multiple -c inputs"
  exit 1
fi
# The merged runtime needs libm on Linux (a no-op on macOS and Windows).
$LINKER multic_b.o multic_a.o $MATHLIB $RTLIBFLAGS -o multic$EXE || { echo "FAIL multic: link"; exit 1; }
./multic$EXE
exit 0
