#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# scripts/bundle_standalone.sh — assert a release binary is standalone.
# Usage: bundle_standalone.sh --assert-static <built-pli-binary>
# Fails if the binary references any toolchain shared library. The
# static-prefix build must ship no lib/ dir at all (ADR-224); the driver is
# fully in-process with no shipped clang (ADR-225), so there is nothing left
# to bundle — this script is a gate, not a packer.
set -u

if [ "${1:-}" != "--assert-static" ]; then
  echo "usage: bundle_standalone.sh --assert-static <binary>" >&2
  exit 1
fi
BIN=${2:-}
if [ -z "$BIN" ] || [ ! -f "$BIN" ]; then echo "FAIL: --assert-static needs a binary" >&2; exit 1; fi

# Windows links statically already (/MT + static LLVM archives): nothing to do.
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*|Windows_NT*) echo "static: $BIN (windows exe already static)"; exit 0 ;;
esac

case "$(uname -s)" in
  Darwin)
    if otool -L "$BIN" | grep -E "libLLVM|liblld|libclang|libz3|libzstd|libxml2|/opt/homebrew|/opt/local|/usr/local/(llvm|lld|zstd|z3)"; then
      echo "FAIL: static binary still has toolchain dylib refs" >&2; exit 1
    fi ;;
  *)
    if ldd "$BIN" | grep -E "libLLVM|liblld|libz3|libzstd|libxml2|/usr/lib/llvm|/opt/|not found"; then
      echo "FAIL: static binary still has toolchain .so refs" >&2; exit 1
    fi ;;
esac
echo "static: $BIN links no toolchain shared libraries"
exit 0
