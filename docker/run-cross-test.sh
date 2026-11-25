#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# docker/run-cross-test.sh — agent entry point for Linux/Windows testing from macOS.
# Linux tests run against a prebuilt static-musl pli binary (built on a Debian
# host with PLI_LLVM_STATIC_PREFIX; TinyCore has no LLVM >= 23 tcz).
# Wine runs Windows .exe files headless.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
LINUX_IMG=${LINUX_IMG:-pli-linux-test}
WINE_IMG=${WINE_IMG:-pli-wine-test}
# PLATFORM="--platform linux/amd64" forces x86_64 emulation on ARM Macs.
PLATFORM=${PLATFORM:-}

usage() {
  echo "usage: $(basename "$0") build|linux-native [test-group]|wine-run <prog.exe>"
  exit 2
}

cmd_build() {
  # Build both test images (run once; rebuild after Dockerfile edits).
  # TinyCore image is a test-runner only — pli itself is built on a Debian host
  # (scripts/fetch_llvm_static.sh + PLI_LLVM_STATIC_PREFIX) and mounted in.
  docker build -f "$ROOT/docker/Dockerfile.linux-test" -t "$LINUX_IMG" "$ROOT/docker"
  docker build -f "$ROOT/docker/Dockerfile.wine-test" -t "$WINE_IMG" "$ROOT/docker"
}

cmd_linux_native() {
  # Smoke-test a prebuilt static-musl pli binary mounted at /work/build/cmake/pli.
  # Build pli first on a Debian host:
  #   sh scripts/fetch_llvm_static.sh --prefix /opt/pli-deps
  #   cmake -G Ninja -S . -B build/cmake -DPLI_LLVM_STATIC_PREFIX=/opt/pli-deps
  #   cmake --build build/cmake -j
  # TinyCore has no CMake/ctest — this only compiles + runs hello.pli.
  # Run the full suite on the host: ctest --test-dir build/cmake -j
  PLI_BIN="$ROOT/build/cmake/pli"
  [ -f "$PLI_BIN" ] || { echo "run-cross-test.sh: $PLI_BIN not found — build pli first (see cmd_linux_native comments)" >&2; exit 1; }
  docker run --rm $PLATFORM -v "$ROOT:/work" -w /work "$LINUX_IMG" \
    sh -c "/work/build/cmake/pli tests/core/hello.pli -o /tmp/hello && /tmp/hello"
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
  wine-run) shift; cmd_wine_run "$@" ;;
  *) usage ;;
esac
