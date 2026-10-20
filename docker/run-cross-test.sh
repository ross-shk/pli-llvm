#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# docker/run-cross-test.sh — agent entry point for Linux/Windows testing from macOS.
# Native Linux tests build pli *inside* the container; the cross tests prepare
# a target sysroot, build a cross-enabled pli, and run the produced binaries
# in the containers (Linux) or under Wine (Windows).
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
LINUX_IMG=${LINUX_IMG:-pli-linux-test}
WINE_IMG=${WINE_IMG:-pli-wine-test}
# RUN_IMG is the lightweight runner for already-built binaries (no toolchain).
RUN_IMG=${RUN_IMG:-debian:trixie-slim}
# PLATFORM="--platform linux/amd64" forces x86_64 emulation on ARM Macs.
PLATFORM=${PLATFORM:-}
# The cross build lives in its own tree: it embeds the per-target runtimes.
CROSS_BUILD=${CROSS_BUILD:-build-cross/cmake}

usage() {
  echo "usage: $(basename "$0") build|linux-native [test-group]|cross|wine-run <prog.exe>"
  exit 2
}

cmd_build() {
  # Build both test images (run once; rebuild after Dockerfile edits).
  docker build -f "$ROOT/docker/Dockerfile.linux-test" -t "$LINUX_IMG" "$ROOT/docker"
  docker build -f "$ROOT/docker/Dockerfile.wine-test" -t "$WINE_IMG" "$ROOT/docker"
}

cmd_linux_native() {
  # Build pli natively on Linux and smoke-test it (default: hello + group).
  # BUILD=build-linux keeps the container build clear of the host build/ dir.
  GROUP=${1:-core}
  docker run --rm $PLATFORM -v "$ROOT:/work" -w /work "$LINUX_IMG" \
    sh -c "cmake -G Ninja -S . -B build-linux/cmake && cmake --build build-linux/cmake -j\$(nproc) && ./build-linux/cmake/pli tests/core/hello.pli -o /tmp/hello && /tmp/hello && ctest --test-dir build-linux/cmake -j\$(nproc) --output-on-failure -R \"${GROUP}\""
}

cmd_cross() {
  # The cross path (ADR-234): prepare the target sysroots, build a pli with
  # the per-target runtime bitcode, then compile and RUN a program for each
  # target: the Linux binaries in the runner container, the Windows one under
  # Wine.
  cd "$ROOT"

  echo "== preparing the cross sysroots"
  for t in x86_64-unknown-linux-gnu aarch64-unknown-linux-gnu x86_64-pc-windows-gnu; do
    sh scripts/prepare_cross_sysroot.sh "$t"
  done

  echo "== building the cross-enabled compiler"
  cmake -G Ninja -S . -B "$CROSS_BUILD" -DPLI_CROSS_BITCODE=ON \
    -DCMAKE_PREFIX_PATH="${CMAKE_PREFIX_PATH:-}" 2>/dev/null ||
    cmake -G Ninja -S . -B "$CROSS_BUILD" -DPLI_CROSS_BITCODE=ON
  cmake --build "$CROSS_BUILD" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

  PLI="$ROOT/$CROSS_BUILD/pli"

  echo "== linux x86_64"
  "$PLI" --triple x86_64-unknown-linux-gnu tests/core/hello.pli -o "$ROOT/$CROSS_BUILD/hello-linux-x86_64"
  docker run --rm $PLATFORM -v "$ROOT:/work" -w "/work/$CROSS_BUILD" "$RUN_IMG" ./hello-linux-x86_64

  echo "== linux aarch64"
  "$PLI" --triple aarch64-unknown-linux-gnu tests/core/hello.pli -o "$ROOT/$CROSS_BUILD/hello-linux-aarch64"
  docker run --rm --platform linux/arm64 -v "$ROOT:/work" -w "/work/$CROSS_BUILD" "$RUN_IMG" \
    ./hello-linux-aarch64

  echo "== windows x86_64"
  "$PLI" --triple x86_64-pc-windows-gnu tests/core/hello.pli -o "$ROOT/$CROSS_BUILD/hello-windows-x86_64.exe"
  docker run --rm $PLATFORM -v "$ROOT:/work" -w "/work/$CROSS_BUILD" "$WINE_IMG" wine64 ./hello-windows-x86_64.exe

  echo "PASS cross"
}

cmd_wine_run() {
  # Run a Windows .exe produced by pli under headless Wine.
  [ $# -ge 1 ] || usage
  EXE=$1
  docker run --rm $PLATFORM -v "$ROOT:/work" -w /work "$WINE_IMG" wine64 "$EXE"
}

case ${1:-} in
  build) shift; cmd_build "$@" ;;
  linux-native) shift; cmd_linux_native "$@" ;;
  cross) shift; cmd_cross "$@" ;;
  wine-run) shift; cmd_wine_run "$@" ;;
  *) usage ;;
esac
