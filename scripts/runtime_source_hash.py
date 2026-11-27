#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
"""scripts/runtime_source_hash.py — deterministic hash of the runtime sources.

The checked-in Darwin runtime blobs (platform/darwin-runtime, ADR-396) are
built from runtime/ on a macOS host with the pinned LLVM + Apple SDK. This
script hashes the runtime SOURCE tree so CMake / CI can tell when the blobs no
longer match the current sources and must be regenerated.

We hash the sources, NOT the .bc bytes: bitcode output is not byte-reproducible
across clang/LLVM patch versions even with identical sources, so a byte hash
would false-positive on any toolchain bump.

Usage:
  runtime_source_hash.py <runtime-dir>   # prints one line "<sha256>  runtime/"

The output line must be byte-for-byte identical to the single line recorded in
platform/darwin-runtime/SHA256SUMS (source-tree hash, not per-file hashes).
"""
import hashlib
import os
import sys


# The file kinds that feed the bitcode embeds (the .c TUs + the headers they
# include transitively). Everything reachable is under runtime/ already.
_SRC_EXTS = (".c", ".h")


def main(runtime_dir: str) -> None:
    if not os.path.isdir(runtime_dir):
        print(f"error: not a directory: {runtime_dir}", file=sys.stderr)
        sys.exit(1)
    # Deterministic across hosts: walk in sorted order, hash canonical paths
    # and bytes. Ignore nothing — every source/header under runtime/ affects
    # the built blobs.
    h = hashlib.sha256()
    h.update(b"pli-runtime-source-tree-v1\n")
    for root, _dirs, files in sorted(os.walk(runtime_dir)):
        for name in sorted(files):
            if not name.endswith(_SRC_EXTS):
                continue
            path = os.path.join(root, name)
            # Canonical slash-separated path relative to the runtime dir.
            rel = os.path.relpath(path, runtime_dir).replace("\\", "/")
            with open(path, "rb") as f:
                data = f.read()
            h.update(b"file ")
            h.update(rel.encode("utf-8"))
            h.update(b"\n")
            h.update(data)
            h.update(b"\n")
    print(f"{h.hexdigest()}  runtime/")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <runtime-dir>", file=sys.stderr)
        sys.exit(1)
    main(sys.argv[1])
