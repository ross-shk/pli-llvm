#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# scripts/build_runtime_for.sh — build the PL/I runtime for a target with the
# user's own toolchain.
#
# The portable bitcode mode (ADR-236) leaves the pli_* symbols undefined: the
# program is finished by the user's toolchain, which needs the runtime built
# for the same target. This script does that build:
#
#   scripts/build_runtime_for.sh e2k-unknown-linux-gnu
#   scripts/build_runtime_for.sh x86_64-pc-windows-gnu clang libpli_runtime.a
#   scripts/build_runtime_for.sh x86_64-unknown-linux-gnu clang out.a --sysroot DIR
#
# A target whose toolchain is complete (a vendor toolchain) needs no
# --sysroot; a target built from a prepared sysroot (see
# scripts/prepare_cross_sysroot.sh) needs its headers, which --sysroot points
# at.
#
# The runtime TUs are exactly the runtime/rt_*.c files (the portable ones plus
# the platform TU). For an OS whose APIs differ from the default platform TU,
# write one against runtime/pli_rt_plat.h and compile it instead of
# rt_plat_default.c (pli links it with --rt-platform).
set -eu

TRIPLE=""
CC=""
OUT=""
SYSROOT=""
POS=0

while [ $# -gt 0 ]; do
  case "$1" in
    --sysroot) SYSROOT="${2:-}"; shift 2 ;;
    -h|--help)
      echo "usage: $(basename "$0") <triple> [cc] [out.a] [--sysroot <dir>]" >&2
      exit 2
      ;;
    *)
      POS=$((POS + 1))
      case "$POS" in
        1) TRIPLE="$1" ;;
        2) CC="$1" ;;
        3) OUT="$1" ;;
        *) echo "usage: $(basename "$0") <triple> [cc] [out.a] [--sysroot <dir>]" >&2; exit 2 ;;
      esac
      shift
      ;;
  esac
done

[ -n "$CC" ] || CC=clang
[ -n "$OUT" ] || OUT=libpli_runtime.a

if [ -z "$TRIPLE" ]; then
  echo "usage: $(basename "$0") <triple> [cc] [out.a] [--sysroot <dir>]" >&2
  exit 2
fi

HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/pli-runtime-XXXXXX")
trap 'rm -rf "$WORK"' EXIT INT TERM

command -v "$CC" >/dev/null 2>&1 || {
  echo "pli: no C compiler '$CC' on PATH" >&2
  exit 1
}

# How the target's headers are found: a sysroot in the musl/mingw layout
# keeps them in include/ (clang's -isysroot would look in usr/include), while
# a real sysroot (the macOS SDK) is passed as one.
INC_FLAGS=""
if [ -n "$SYSROOT" ]; then
  if [ -d "$SYSROOT/include" ]; then
    INC_FLAGS="-isystem $SYSROOT/include"
  else
    INC_FLAGS="-isysroot $SYSROOT"
  fi
fi

for src in "$HERE"/runtime/rt_*.c; do
  base=$(basename "$src" .c)
  # shellcheck disable=SC2086
  "$CC" -target "$TRIPLE" -O2 -I"$HERE/runtime" $INC_FLAGS -c "$src" -o "$WORK/$base.o"
done

ar crs "$OUT" "$WORK"/*.o
echo "runtime for $TRIPLE: $OUT"
echo "  link it with: $CC -target $TRIPLE <program>.bc $OUT -o <program>"
