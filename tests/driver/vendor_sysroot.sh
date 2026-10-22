#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/vendor_sysroot.sh — vendored sysroots (ADR-227): user programs
# link with nothing except the OS. The sysroot is EMBEDDED in pli(.exe) —
# release archives hold only the binary — and materializes to a temp dir at
# link time; an exe-relative dir next to the binary is a dev override.
# Phase 1 stages the binary ALONE (the release layout) and proves the
# embedded path; phase 2 stages it with the platform/ dirs and proves the
# override path. Copies pli to a temp dir so the build tree stays clean.
# The float program exercises libm linkage on every OS (musl stubs, mingw
# import libs, .tbd stubs).
#
# The program is also the anti-drift gate for the vendored sysroot's symbol
# set: it calls every math transcendental, the complex power (which reaches
# exp/log/atan2/sincos), INDEX (memchr) and COPY (the memset_pattern family).
# A stub set that does not export those symbols fails the link here — the
# program used to do plain arithmetic, which constant-folds and needed no
# libm symbol at all, so an incomplete stub set passed unnoticed.
set -u
PLI=${PLI:-./build/cmake/pli}
PLI_DIR=$(dirname "$PLI")
OUT=tests/driver/out/vendor_sysroot
mkdir -p tests/driver/out

cat > "$OUT.pli" <<'EOF'
  sysroot: proc options(main);
     declare x fixed bin(31);
     declare y float bin(53);
     declare z float bin(53);
     declare w float bin(53);
     declare (r, c) complex;
     declare s char(32);
     declare t char(6);
     declare ok fixed bin(31);
     x = 21;
     y = x * 2.0;
     z = 0.5;
     c = complex(1.0, 1.0);
     r = c ** 2;
     s = 'the quick brown fox';
     t = copy('ab', 3);
     w = floor(z) + ceil(z) + sqrt(z) + exp(z) + log(z);
     w = w + sin(z) + cos(z) + tan(z) + log2(z) + log10(z);
     w = w + atan(z) + sinh(z) + cosh(z) + tanh(z) + atanh(z);
     w = w + erf(z) + erfc(z) + asin(z) + acos(z) + cbrt(z);
     w = w + atan2(z, 1.0) + sind(z) + cosd(z) + tand(z) + atand(z);
     ok = 0;
     if abs(real(r)) < 1e-6
        then if abs(imag(r) - 2.0) < 1e-6
           then if w > -1000.0 then ok = 1;
     put skip list('PASS vendor_sysroot', x, y, ok,
                   index(s, 'quick'), length(t));
  end sysroot;
EOF
EXPECT="PASS vendor_sysroot 21 42 1 5 6"

ok=1
fail() { echo "FAIL: $1"; ok=0; }
# Stage the binary alone (phase 1: the release layout) or with the
# platform/ dirs (phase 2: the dev override); DLLs ride along because dev
# builds link shared zlib/zstd (release-static needs none).
stage() {
  rm -rf "$OUT.stage"
  mkdir -p "$OUT.stage"
  cp "$PLI" "$OUT.stage/$PLI_EXE" || { fail "stage pli copy"; return 1; }
  for dll in "$PLI_DIR"/*.dll ./build/cmake/*.dll; do
    [ -f "$dll" ] || continue
    cp "$dll" "$OUT.stage/" 2>/dev/null || true
  done
  if [ "$1" = "single" ]; then
    if [ -e "$OUT.stage/musl" ] || [ -e "$OUT.stage/mingw" ] || [ -e "$OUT.stage/sdk" ]; then
      fail "sysroot dir leaked into single-file stage"
      return 1
    fi
  else
    cp -r "$OVERRIDE_SRC" "$OUT.stage/$OVERRIDE_DST" \
      || { fail "stage $OVERRIDE_DST copy"; return 1; }
  fi
}
# $1 = phase, $2 = extra env assignments for the link, $3 = expected -v marker
link_and_run() {
  cmd=$(env $2 "$OUT.stage/$PLI_EXE" "$OUT.pli" -o "$OUT.bin$EXE" -v 2>&1) \
    || { fail "$1 compile+link"; printf '%s\n' "$cmd"; return 1; }
  printf '%s\n' "$cmd" | grep -Eqil "$3" \
    || { fail "$1 sysroot not picked up ($3)"; return 1; }
  printf '%s\n' "$cmd" | grep -Eqil "$HOST_LEAK" && { fail "$1 host input leaked ($HOST_LEAK)"; return 1; }
  if [ -n "$STATIC_CHECK" ] && command -v file >/dev/null 2>&1; then
    file "$OUT.bin$EXE" | grep -qi 'statically linked' \
      || { fail "$1 binary is not static ($(file "$OUT.bin$EXE"))"; return 1; }
  fi
  got=$("$OUT.bin$EXE" 2>/dev/null | tr -d '\r')
  [ "$got" = "$EXPECT" ] || { fail "$1 run (got: $got)"; return 1; }
}

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*|Windows_NT*)
    # --- Windows: mingw-w64, no MSVC ---
    EXE=.exe
    PLI_EXE=pli.exe
    SYSROOT=platform/windows-mingw
    OVERRIDE_SRC=$SYSROOT
    OVERRIDE_DST=mingw
    HOST_LEAK='Microsoft Visual Studio|/out:|kernel32\.lib|clang_rt\.builtins'
    STATIC_CHECK=
    # Keep this list in step with detectMingw() (src/hostcrt.cpp): a file it
    # requires but this directory lacks makes the override silently fall back.
    for f in crt2.o crtbegin.o crtend.o fltused.o chkstk.o libgcc.a libkernel32.a libmsvcrt.a libmingw32.a libmingwex.a libmoldname.a libucrt.a libadvapi32.a libshell32.a libuser32.a libws2_32.a; do
      [ -f "$SYSROOT/$f" ] || { echo "FAIL: sysroot source missing ($SYSROOT/$f)"; exit 1; }
    done
    if stage single; then link_and_run single "LIB=" 'vendored.*mingw.*embedded'; fi
    if stage override; then link_and_run override "LIB=" 'vendored.*mingw.*override'; fi
    ;;
  Darwin*)
    # --- macOS: SDK stubs, no Xcode CLT in the picture ---
    EXE=
    PLI_EXE=pli
    SYSROOT=platform/darwin-sdk
    OVERRIDE_SRC=$SYSROOT
    OVERRIDE_DST=sdk
    HOST_LEAK='MacOSX\.sdk|Xcode'
    STATIC_CHECK=
    for f in usr/lib/libm.tbd usr/lib/libSystem.tbd; do
      [ -f "$SYSROOT/$f" ] || { echo "FAIL: sysroot source missing ($SYSROOT/$f)"; exit 1; }
    done
    if stage single; then link_and_run single "" 'vendored SDK stubs.*embedded'; fi
    if stage override; then link_and_run override "" 'vendored SDK stubs.*override'; fi
    ;;
  *)
    # --- Linux: musl static, no host CRT ---
    EXE=
    PLI_EXE=pli
    SYSROOT=platform/linux-musl/$(uname -m)
    OVERRIDE_SRC=$SYSROOT
    OVERRIDE_DST=musl
    HOST_LEAK='/usr/lib.*Scrt1\.o|/usr/lib64'
    STATIC_CHECK=1
    for f in crt1.o crti.o crtn.o libc.a; do
      [ -f "$SYSROOT/$f" ] || { echo "FAIL: sysroot source missing ($SYSROOT/$f)"; exit 1; }
    done
    if ! ls "$SYSROOT"/libpli_builtins-*.a >/dev/null 2>&1; then
      echo "FAIL: sysroot source missing (libpli_builtins-<arch>.a)"
      exit 1
    fi
    # Hide the host CRT: point PLI_SYSROOT at an empty dir so only the
    # vendored sysroot can satisfy the link (proves OS-only independence).
    mkdir -p "$OUT.emptyroot"
    if stage single; then link_and_run single "PLI_SYSROOT=$OUT.emptyroot" 'vendored.*musl.*embedded'; fi
    if stage override; then link_and_run override "PLI_SYSROOT=$OUT.emptyroot" 'vendored.*musl.*override'; fi
    ;;
esac

if [ "$ok" -eq 1 ]; then
  echo PASS
  exit 0
else
  exit 1
fi
