#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
"""scripts/gen_darwin_runtime.py — build the vendored Darwin runtime blobs.

Runs ONLY on a macOS host with the Apple SDK and a pinned LLVM 23 toolchain:
a Windows/Linux host cannot compile the PL/I runtime for Apple because the SDK
headers are absent. The output (platform/darwin-runtime) is checked in as
release data and embedded by CMake in every build (ADR-396), so macOS
cross-linking works on any host with no Apple SDK.

For each arch (arm64/x86_64) it compiles the portable+platform runtime TUs to
bitcode exactly as CMakeLists.txt's embedded-runtime rules do, but with Apple
deployment targets + -isysroot. Then it links:
  portable TUs  -> <out>/<arch>/pli_runtime.bc
  platform TU   -> <out>/<arch>/pli_rtplat.bc
and writes <out>/SHA256SUMS with the SOURCE-tree hash (see
runtime_source_hash.py) so the freshness gate in CMake / CI can catch stale
blobs.

Usage:
  gen_darwin_runtime.py --llvm-bindir <bin> [--sdk <MacOSX.sdk>] [--out platform/darwin-runtime]
  --sdk     defaults to $SDKROOT, else `xcrun --show-sdk-path`.
  --version llvm version stamp to bake in (default 23.1.3, the pinned LLVM).
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys

# The split-runtime split (must match CMakeLists.txt RT_PORTABLE_SRCS /
# RT_PLATFORM_SRCS exactly; the portable set links into pli_runtime.bc).
PORTABLE = [
    "rt_core.c", "rt_stream.c", "rt_string.c", "rt_math.c", "rt_mathport.c",
    "rt_cond.c", "rt_storage.c", "rt_get.c", "rt_file.c", "rt_edit.c",
    "rt_task.c", "rt_sync.c", "rt_input.c", "rt_fbroker.c", "rt_sched.c",
    "rt_fiber_asm.c",
]
PLATFORM = ["rt_plat_default.c"]

# arch -> deployment target (the same targets tested in tests/driver).
TARGETS = {
    "arm64": "arm64-apple-macosx15.0",
}


def run(cmd, **kw):
    print("+", " ".join(cmd), file=sys.stderr)
    return subprocess.run(cmd, check=True, **kw)


def source_hash(runtime_dir):
    env = dict(os.environ)
    env["PYTHONPATH"] = os.path.dirname(os.path.abspath(__file__))
    out = subprocess.run(
        [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                      "runtime_source_hash.py"), runtime_dir],
        check=True, capture_output=True, text=True, env=env)
    return out.stdout.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--llvm-bindir", required=True,
                    help="directory holding the pinned LLVM clang/llvm-link")
    ap.add_argument("--sdk", default=os.environ.get("SDKROOT", ""),
                    help="path to MacOSX.sdk (default: SDKROOT or xcrun)")
    ap.add_argument("--out", default="platform/darwin-runtime")
    ap.add_argument("--version", default="23.1.3", help="pli_llvm_version stamp")
    args = ap.parse_args()

    if sys.platform != "darwin":
        print("error: gen_darwin_runtime.py must run on a macOS host (Apple SDK required)",
              file=sys.stderr)
        sys.exit(1)

    sdk = args.sdk
    if not sdk:
        sdk = subprocess.run(["xcrun", "--show-sdk-path"], check=True,
                             capture_output=True, text=True).stdout.strip()
    if not os.path.isdir(sdk):
        print(f"error: SDK not found at: {sdk}", file=sys.stderr)
        sys.exit(1)

    clang = os.path.join(args.llvm_bindir, "clang")
    llvm_link = os.path.join(args.llvm_bindir, "llvm-link")
    if not (os.path.exists(clang) and os.path.exists(llvm_link)):
        print(f"error: LLVM tools not found in {args.llvm_bindir}", file=sys.stderr)
        sys.exit(1)

    # Runtime tree is the repo's runtime/ dir.
    here = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.dirname(here)
    runtime = os.path.join(repo_root, "runtime")

    os.makedirs(args.out, exist_ok=True)
    for arch, target in TARGETS.items():
        archdir = os.path.join(args.out, arch)
        stagedir = os.path.join(archdir, "bc")
        os.makedirs(stagedir, exist_ok=True)
        # Portable TUs -> staged bc, then link to pli_runtime.bc. The staging
        # subdir keeps the llvm-link glob from self-matching the output file.
        for src in PORTABLE:
            run([clang, "-emit-llvm", "-c", os.path.join(runtime, src),
                 f'-DPLI_LLVM_VERSION="{args.version}"',
                 "-I", runtime, "-O2",
                 "-ffunction-sections", "-fdata-sections", "-fPIC",
                 "-target", target, "-isysroot", sdk,
                 "-o", os.path.join(stagedir, os.path.splitext(src)[0] + ".bc")])
        objs = [os.path.join(stagedir, os.path.splitext(s)[0] + ".bc") for s in PORTABLE]
        run([llvm_link] + objs + ["-o", os.path.join(archdir, "pli_runtime.bc")])
        # Platform TU -> pli_rtplat.bc.
        run([clang, "-emit-llvm", "-c", os.path.join(runtime, PLATFORM[0]),
             f'-DPLI_LLVM_VERSION="{args.version}"',
             "-I", runtime, "-O2",
             "-ffunction-sections", "-fdata-sections", "-fPIC",
             "-target", target, "-isysroot", sdk,
             "-o", os.path.join(archdir, "pli_rtplat.bc")])
        shutil.rmtree(stagedir, ignore_errors=True)

    # Record the source-tree hash for the freshness gate.
    with open(os.path.join(args.out, "SHA256SUMS"), "w") as f:
        f.write(source_hash(runtime) + "\n")
    print(f"wrote Darwin runtimes + SHA256SUMS under {args.out}")


if __name__ == "__main__":
    main()
