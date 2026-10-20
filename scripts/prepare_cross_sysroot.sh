#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# scripts/prepare_cross_sysroot.sh — build a link sysroot for a cross target.
#
# A cross target needs two things the build host does not have:
#   - the target's C library headers (to compile the PL/I runtime bitcode);
#   - the target's CRT objects and system libraries (to link the program).
# This script produces both in a container and installs them into a local
# cache; CMake reads the headers (PLI_CROSS_BITCODE=ON) and the driver reads
# the libraries (the cache is probed automatically, or pass --sysroot).
#
# usage: scripts/prepare_cross_sysroot.sh <target> [--out <dir>] [--force]
#                                            [--source docker|download]
#   target: x86_64-unknown-linux-gnu | aarch64-unknown-linux-gnu |
#           x86_64-pc-windows-gnu
#   --out: install directory (default: $PLI_SYSROOT_DIR or ~/.pli/sysroots/<target>)
#   --force: rebuild even when the cache already holds a sysroot
#   --source: how to get the target toolchain:
#     docker   (default) install the distribution packages in a container
#     download fetch a prebuilt toolchain (no docker needed)
#
# The produced layout is canonical, so the driver needs no per-distro
# knowledge:
#   <out>/include/...        target libc headers
#   <out>/lib/crt1.o crti.o crtn.o libc.a libm.a libpthread.a libdl.a librt.a
#   <out>/lib/libpli_builtins-<arch>.a   compiler-rt 128-bit helpers, renamed
#   <out>/lib/crt2.o crtbegin.o crtend.o fltused.o chkstk.o lib*.a  (mingw)
#
# The macOS SDK is not redistributable and cannot be prepared here: a Darwin
# cross target uses the SDK of the build host (xcrun --show-sdk-path).
set -eu

TARGET=""
OUT=""
FORCE=0
SOURCE=docker

usage() {
  echo "usage: $(basename "$0") <target> [--out <dir>] [--force] [--source docker|download]" >&2
  echo "targets: x86_64-unknown-linux-gnu aarch64-unknown-linux-gnu x86_64-pc-windows-gnu" >&2
  exit 2
}

while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT="${2:-}"; shift 2 ;;
    --force) FORCE=1; shift ;;
    --source) SOURCE="${2:-}"; shift 2 ;;
    -h|--help) usage ;;
    -*) echo "unknown option: $1" >&2; usage ;;
    *) [ -z "$TARGET" ] || usage; TARGET="$1"; shift ;;
  esac
done
[ -n "$TARGET" ] || usage
case "$SOURCE" in
  docker|download) ;;
  *) echo "pli: unknown --source '$SOURCE' (docker|download)" >&2; exit 2 ;;
esac

CACHE_ROOT="${PLI_SYSROOT_DIR:-$HOME/.pli/sysroots}"
[ -n "$OUT" ] || OUT="$CACHE_ROOT/$TARGET"

# The arch the builtins archive is named for, and the container recipe.
case "$TARGET" in
  x86_64-unknown-linux-gnu)
    ARCH=x86_64
    IMAGE="debian:trixie-slim"
    PLATFORM_ARG=""
    PACKAGES="musl-tools musl-dev clang"
    ;;
  aarch64-unknown-linux-gnu)
    ARCH=aarch64
    IMAGE="ubuntu:24.04"
    PLATFORM_ARG="--platform linux/arm64"
    PACKAGES="musl-dev clang"
    ;;
  x86_64-pc-windows-gnu)
    ARCH=x86_64
    IMAGE="debian:trixie-slim"
    PLATFORM_ARG=""
    PACKAGES="mingw-w64 gcc-mingw-w64-x86-64 clang"
    ;;
  aarch64-apple-macosx15.0)
    echo "pli: a macOS target needs the SDK of the build host (xcrun --show-sdk-path);" >&2
    echo "     it cannot be prepared into a cache (the SDK is not redistributable)." >&2
    exit 2
    ;;
  *)
    echo "pli: unknown target '$TARGET'" >&2
    usage
    ;;
esac

if [ -d "$OUT/lib" ] && [ "$FORCE" -eq 0 ]; then
  echo "sysroot for $TARGET already at $OUT (use --force to rebuild)"
  exit 0
fi

# --- the download source -------------------------------------------------
# A prebuilt toolchain, for the hosts without a docker daemon. The musl
# toolchains carry the target's headers, its libc and its crt objects; the
# mingw one carries the headers and the import libraries.
prepare_download() {
  work=$(mktemp -d "${TMPDIR:-/tmp}/pli-sysroot-dl-XXXXXX")
  trap 'rm -rf "$work"' EXIT INT TERM
  mkdir -p "$OUT/include" "$OUT/lib"
  case "$TARGET" in
    x86_64-unknown-linux-gnu)
      url=https://musl.cc/x86_64-linux-musl-native.tgz
      root=x86_64-linux-musl-native
      ;;
    aarch64-unknown-linux-gnu)
      url=https://musl.cc/aarch64-linux-musl-cross.tgz
      root=aarch64-linux-musl-cross
      ;;
    x86_64-pc-windows-gnu)
      url=$(curl -sSL --max-time 60 https://api.github.com/repos/mstorsjo/llvm-mingw/releases/latest |
        sed -n 's/.*"browser_download_url": "\([^"]*ucrt-x86_64\.zip\)".*/\1/p' | head -1)
      [ -n "$url" ] || {
        echo "pli: cannot find the llvm-mingw release" >&2
        exit 1
      }
      ;;
  esac
  echo "downloading $url"
  archive="$work/toolchain"
  curl -sSL --max-time 1800 -o "$archive" "$url"
  case "$url" in
    *.zip)
      # unzip is not everywhere; python3 is (it is a build dependency).
      if command -v unzip >/dev/null 2>&1; then
        (cd "$work" && unzip -q "$archive")
      else
        (cd "$work" && python3 -c "import sys,zipfile; zipfile.ZipFile(sys.argv[1]).extractall()" "$archive")
      fi
      ;;
    *) (cd "$work" && tar xf "$archive") ;;
  esac
  case "$TARGET" in
    x86_64-unknown-linux-gnu)
      base=$(find "$work" -maxdepth 1 -type d -name 'x86_64-linux-musl*' | head -1)
      cp -r "$base/include/." "$OUT/include/"
      cp -f "$base/lib/libc.a" "$base/lib/crt1.o" "$base/lib/crti.o" "$base/lib/crtn.o" "$OUT/lib/"
      for l in libm.a libpthread.a libdl.a librt.a; do : > "$OUT/lib/$l"; done
      # The 128-bit helpers musl omits: the toolchain's libgcc provides them.
      cp -f "$(find "$base" -name libgcc.a | head -1)" "$OUT/lib/libpli_builtins-x86_64.a"
      ;;
    aarch64-unknown-linux-gnu)
      base=$(find "$work" -maxdepth 1 -type d -name 'aarch64-linux-musl*' | head -1)
      cp -r "$base/aarch64-linux-musl/include/." "$OUT/include/"
      cp -f "$base/aarch64-linux-musl/lib/libc.a" "$base/aarch64-linux-musl/lib/crt1.o" \
            "$base/aarch64-linux-musl/lib/crti.o" "$base/aarch64-linux-musl/lib/crtn.o" "$OUT/lib/"
      for l in libm.a libpthread.a libdl.a librt.a; do : > "$OUT/lib/$l"; done
      # The cross toolchain's libgcc is the host's; the target's 128-bit
      # helpers come from the target compiler-rt when it is available.
      builtins=$(find "$base" -name 'libgcc.a' -path '*aarch64*' | head -1)
      [ -n "$builtins" ] && cp -f "$builtins" "$OUT/lib/libpli_builtins-aarch64.a"
      ;;
    x86_64-pc-windows-gnu)
      base=$(find "$work" -maxdepth 2 -type d -name 'x86_64-w64-mingw32' | head -1)
      [ -n "$base" ] || base=$(find "$work" -maxdepth 3 -type d -name 'x86_64-w64-mingw32' | head -1)
      [ -n "$base" ] || {
        echo "pli: the llvm-mingw archive has no x86_64-w64-mingw32 directory" >&2
        exit 1
      }
      cp -r "$base/include/." "$OUT/include/"
      # A missing file is skipped, not fatal (the set differs per release).
      for f in crt2.o crtbegin.o crtend.o libkernel32.a libmsvcrt.a libmingw32.a \
               libmingwex.a libmoldname.a libucrt.a libadvapi32.a libshell32.a \
               libuser32.a libgcc.a; do
        if [ -f "$base/lib/$f" ]; then
          cp -f "$base/lib/$f" "$OUT/lib/"
        fi
      done
      # The two CRT build products the distribution does not ship: the host
      # clang cross-compiles them (their sources are in platform/windows-mingw).
      src_dir="$(CDPATH= cd -- "$(dirname -- "$0")/../platform/windows-mingw" && pwd)"
      "$CC_FOR_TOOLS" -target x86_64-pc-windows-gnu -c "$src_dir/fltused.c" -o "$OUT/lib/fltused.o"
      "$CC_FOR_TOOLS" -target x86_64-pc-windows-gnu -c "$src_dir/chkstk.s" -o "$OUT/lib/chkstk.o"
      ;;
  esac
}

if [ "$SOURCE" = "download" ]; then
  CC_FOR_TOOLS=${CC_FOR_TOOLS:-clang}
  command -v "$CC_FOR_TOOLS" >/dev/null 2>&1 || CC_FOR_TOOLS=cc
  prepare_download
  echo "sysroot for $TARGET ready at $OUT"
  echo "  headers: $OUT/include"
  echo "  libs:    $OUT/lib"
  exit 0
fi

command -v docker >/dev/null 2>&1 || {
  echo "pli: docker is required to prepare a cross sysroot (the container holds" >&2
  echo "     the target toolchain); install Docker and retry, or use" >&2
  echo "     --source download for a prebuilt toolchain" >&2
  exit 1
}
docker info >/dev/null 2>&1 || {
  echo "pli: the docker daemon is not running (start Docker Desktop and retry)" >&2
  exit 1
}

STAGE=$(mktemp -d "${TMPDIR:-/tmp}/pli-sysroot-XXXXXX")
trap 'rm -rf "$STAGE"' EXIT INT TERM
mkdir -p "$STAGE/out" "$STAGE/src"

# The mingw CRT needs two small build products that the distribution does not
# ship: fltused.o (the MSVC _fltused marker) and chkstk.o (the stack-probe
# thunk). Their sources live in platform/windows-mingw; they are cross-built
# inside the container.
if [ "$TARGET" = "x86_64-pc-windows-gnu" ]; then
  SRC_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/../platform/windows-mingw" && pwd)"
  cp "$SRC_DIR/fltused.c" "$SRC_DIR/chkstk.s" "$STAGE/src/" || exit 1
fi

echo "preparing $TARGET in $IMAGE ..."

# The container installs the target toolchain and copies the sysroot into
# /out. The arch-specific paths differ per distribution, so the copy is done
# with a small shell script inside the container.
docker run --rm $PLATFORM_ARG -v "$STAGE:/stage" "$IMAGE" sh -euxc "
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq --no-install-recommends $PACKAGES ca-certificates
  mkdir -p /stage/out/include /stage/out/lib
  case '$TARGET' in
    x86_64-unknown-linux-gnu)
      cp -r /usr/include/x86_64-linux-musl/. /stage/out/include/
      cp -f /usr/lib/x86_64-linux-musl/crt1.o /usr/lib/x86_64-linux-musl/crti.o \
            /usr/lib/x86_64-linux-musl/crtn.o /usr/lib/x86_64-linux-musl/libc.a /stage/out/lib/
      for l in libm.a libpthread.a libdl.a librt.a; do
        : > /stage/out/lib/\$l
      done
      ;;
    aarch64-unknown-linux-gnu)
      cp -r /usr/lib/aarch64-linux-musl/include/. /stage/out/include/
      cp -f /usr/lib/aarch64-linux-musl/crt1.o /usr/lib/aarch64-linux-musl/crti.o \
            /usr/lib/aarch64-linux-musl/crtn.o /usr/lib/aarch64-linux-musl/libc.a /stage/out/lib/
      for l in libm.a libpthread.a libdl.a librt.a; do
        : > /stage/out/lib/\$l
      done
      ;;
    x86_64-pc-windows-gnu)
      cp -r /usr/x86_64-w64-mingw32/include/. /stage/out/include/
      cp -f /usr/x86_64-w64-mingw32/lib/crt2.o /usr/x86_64-w64-mingw32/lib/crtbegin.o \
            /usr/x86_64-w64-mingw32/lib/crtend.o /stage/out/lib/
      cp -f /usr/lib/gcc/x86_64-w64-mingw32/*/libgcc.a /stage/out/lib/ 2>/dev/null || true
      for l in libkernel32.a libmsvcrt.a libmingw32.a libmingwex.a libmoldname.a \\
               libucrt.a libadvapi32.a libshell32.a libuser32.a; do
        cp -f /usr/x86_64-w64-mingw32/lib/\$l /stage/out/lib/ 2>/dev/null || true
      done
      # The two CRT build products the distribution does not ship.
      clang -target x86_64-pc-windows-gnu -c /stage/src/fltused.c -o /stage/out/lib/fltused.o
      clang -target x86_64-pc-windows-gnu -c /stage/src/chkstk.s -o /stage/out/lib/chkstk.o
      ;;
  esac
  # The 128-bit helpers musl omits (wide-decimal formatting): compiler-rt's
  # builtins, renamed so the hostcrt clang-shadow check never trips on it.
  for b in \$(find /usr/lib/llvm-*/lib/clang/*/lib -name 'libclang_rt.builtins-*.a' 2>/dev/null); do
    case \"\$b\" in
      *-$ARCH.a) cp -f \"\$b\" /stage/out/lib/libpli_builtins-$ARCH.a; break ;;
    esac
  done
"

rm -rf "$OUT"
mkdir -p "$OUT"
cp -r "$STAGE/out/." "$OUT/"

# fltused.o (the MSVC _fltused marker) and chkstk.o (the stack probe thunk)
# are small build products: the sources live in platform/windows-mingw.
if [ "$TARGET" = "x86_64-pc-windows-gnu" ]; then
  if [ ! -f "$OUT/lib/fltused.o" ] || [ ! -f "$OUT/lib/chkstk.o" ]; then
    echo "pli: the mingw sysroot is incomplete (fltused.o / chkstk.o missing)" >&2
    exit 1
  fi
fi

if [ ! -f "$OUT/lib/libc.a" ] && [ ! -f "$OUT/lib/libkernel32.a" ]; then
  echo "pli: the prepared sysroot at $OUT has no libc" >&2
  exit 1
fi

echo "sysroot for $TARGET ready at $OUT"
echo "  headers: $OUT/include"
echo "  libs:    $OUT/lib"
