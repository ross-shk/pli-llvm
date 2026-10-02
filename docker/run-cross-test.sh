#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# docker/run-cross-test.sh — agent entry point for Linux/Windows testing from macOS.
# Native Linux tests build plic *inside* the container (host plic emits Mach-O
# and cross-bitcode is not ready yet); Wine runs Windows .exe files headless.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
LINUX_IMG=${LINUX_IMG:-pli-linux-test}
WINE_IMG=${WINE_IMG:-pli-wine-test}
# RUN_IMG is the lightweight runner for already-built binaries (no toolchain).
RUN_IMG=${RUN_IMG:-debian:trixie-slim}
# PLATFORM="--platform linux/amd64" forces x86_64 emulation on ARM Macs.
PLATFORM=${PLATFORM:-}

usage() {
  echo "usage: $(basename "$0") build|linux-native [test-group]|wine-run <prog.exe>"
  exit 2
}

cmd_build() {
  # Build both test images (run once; rebuild after Dockerfile edits).
  docker build -f "$ROOT/docker/Dockerfile.linux-test" -t "$LINUX_IMG" "$ROOT/docker"
  docker build -f "$ROOT/docker/Dockerfile.wine-test" -t "$WINE_IMG" "$ROOT/docker"
}

cmd_linux_native() {
  # Build plic natively on Linux and smoke-test it (default: hello + group).
  # BUILD=build-linux keeps the container build clear of the host build/ dir.
  GROUP=${1:-core}
  docker run --rm $PLATFORM -v "$ROOT:/work" -w /work "$LINUX_IMG" \
    sh -c "make BUILD=build-linux -j\$(nproc) && ./build-linux/plic tests/core/hello.pli -o /tmp/hello && /tmp/hello && PLIC=./build-linux/plic CLANG=clang-22 python3 tests/run_tests.py $GROUP"
}

cmd_wine_run() {
  # Run a Windows .exe produced by plic under headless Wine.
  [ $# -ge 1 ] || usage
  EXE=$1
  docker run --rm $PLATFORM -v "$ROOT:/work" -w /work "$WINE_IMG" wine64 "$EXE"
}

case ${1:-} in
  build) shift; cmd_build "$@" ;;
  linux-native) shift; cmd_linux_native "$@" ;;
  wine-run) shift; cmd_wine_run "$@" ;;
  *) usage ;;
esac
