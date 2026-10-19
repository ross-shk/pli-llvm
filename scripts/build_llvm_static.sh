#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# scripts/build_llvm_static.sh — one-time hermetic build of the static LLVM
# toolchain (zlib + zstd + LLVM/clang/lld, all static archives, no shared
# libLLVM, no z3, no libxml2). Runs ONLY when a pin is bumped, via
# .github/workflows/llvm-deps.yml (workflow_dispatch) or by hand.
#
# Normal CI and dev machines use scripts/fetch_llvm_static.sh instead and
# never compile LLVM. After a successful run, paste the printed sha256 into
# scripts/llvm_static.pins and commit pins + stash together.
#
# Usage: build_llvm_static.sh --prefix <dir> [--pack <out.tar.zst>]
#                             [--jobs N] [--workdir <dir>]
set -u

PREFIX= PACK= JOBS= WORKDIR=
while [ "$#" -gt 0 ]; do
  case "$1" in
    --prefix) PREFIX=$2; shift 2 ;;
    --pack) PACK=$2; shift 2 ;;
    --jobs) JOBS=$2; shift 2 ;;
    --workdir) WORKDIR=$2; shift 2 ;;
    *) echo "build_llvm_static.sh: unknown arg $1" >&2; exit 2 ;;
  esac
done
[ -n "$PREFIX" ] || { echo "build_llvm_static.sh: --prefix required" >&2; exit 2; }

SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PINS=$SELF_DIR/llvm_static.pins
pin() { awk -F= -v k="$1" '$1==k {print $2; exit}' "$PINS"; }
LLVM_VER=$(pin LLVM_VER) LLVM_TAG=$(pin LLVM_TAG) LLVM_SRC_SHA256=$(pin LLVM_SRC_SHA256)
ZLIB_VER=$(pin ZLIB_VER) ZSTD_VER=$(pin ZSTD_VER)
[ -n "$LLVM_VER" ] || { echo "build_llvm_static.sh: pins unreadable" >&2; exit 2; }

[ -n "$JOBS" ] || JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
[ -n "$WORKDIR" ] || WORKDIR=${TMPDIR:-/tmp}/pli-llvm-deps
# --pack is relative to the directory the caller invoked us from (e.g. the CI
# workspace), NOT the scratch build dir we cd into below.
ORIG_DIR=$(pwd)
mkdir -p "$WORKDIR" "$PREFIX"
cd "$WORKDIR"

# Windows (git-bash): no static zlib recipe (zlib has no CMakeLists and its
# ./configure needs a POSIX cc), so LLVM is built with zlib OFF there; zstd
# (real CMake project) is still static everywhere. Documented in the ADR.
# The driver ships /MT (static CRT, CMakeLists MSVC_RUNTIME_LIBRARY), and
# MSVC emits a /failifmismatch directive for the CRT: LLVM and zstd must be
# built MultiThreaded too or linking pli fails (MT_StaticRelease vs
# MD_DynamicRelease). zstd's CMakeLists predates CMP0091, so force the policy
# NEW for its runtime property to take effect.
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*|Windows_NT*)
    WITH_ZLIB=OFF
    # LLVM 23 removed LLVM_USE_CRT_*; the runtime comes from the standard
    # CMAKE_MSVC_RUNTIME_LIBRARY variable (CMP0091). Setting the policy
    # default to NEW makes it apply even in subprojects that predate it.
    CRT_ARGS="-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_POLICY_DEFAULT_CMP0091=NEW"
    ;;
  *)
    WITH_ZLIB=FORCE_ON
    CRT_ARGS=""
    ;;
esac

fetch() { # fetch <url> <file>
  [ -f "$2" ] || curl -fL --retry 3 -o "$2" "$1" \
    || { echo "build_llvm_static.sh: download failed: $1" >&2; exit 1; }
}
if command -v sha256sum >/dev/null 2>&1; then
  SHASUM() { sha256sum "$1" 2>/dev/null | awk '{print $1}'; }
else
  SHASUM() { shasum -a 256 "$1" 2>/dev/null | awk '{print $1}'; }
fi

# LLVM sources (tarball, not a clone: no git history, no submodules).
fetch "https://github.com/llvm/llvm-project/releases/download/$LLVM_TAG/llvm-project-$LLVM_VER.src.tar.xz" \
  "llvm-project-$LLVM_VER.src.tar.xz"
[ "$(SHASUM "llvm-project-$LLVM_VER.src.tar.xz")" = "$LLVM_SRC_SHA256" ] \
  || { echo "build_llvm_static.sh: LLVM tarball checksum mismatch" >&2; exit 1; }
[ -d "llvm-project-$LLVM_VER.src" ] || tar -xf "llvm-project-$LLVM_VER.src.tar.xz"

# zlib (static only; skipped on Windows, see above). Use the /fossils archive:
# zlib.net keeps only the current release at the root, so a pinned older
# version 404s there but stays forever under fossils (version-pinned URL).
if [ "$WITH_ZLIB" = "FORCE_ON" ]; then
  fetch "https://zlib.net/fossils/zlib-$ZLIB_VER.tar.gz" "zlib-$ZLIB_VER.tar.gz"
  [ -d "zlib-$ZLIB_VER" ] || tar -xf "zlib-$ZLIB_VER.tar.gz"
  if [ ! -f "$PREFIX/lib/libz.a" ]; then
    # -fPIC: clang's libclang-cpp.so links this archive, and x86_64 refuses
    # non-PIC relocations (R_X86_64_PC32) in a shared object. AArch64 and
    # Apple code is PIC by default, so the failure only shows on Linux/x86_64.
    (cd "zlib-$ZLIB_VER" && CFLAGS="${CFLAGS:--O3} -fPIC" ./configure --static \
      --prefix="$PREFIX" && make -j"$JOBS" && make install) \
      || { echo "build_llvm_static.sh: zlib build failed" >&2; exit 1; }
  fi
fi

# zstd (static only, all platforms incl. Windows).
fetch "https://github.com/facebook/zstd/releases/download/v$ZSTD_VER/zstd-$ZSTD_VER.tar.gz" \
  "zstd-$ZSTD_VER.tar.gz"
[ -d "zstd-$ZSTD_VER" ] || tar -xf "zstd-$ZSTD_VER.tar.gz"
if [ ! -f "$PREFIX/lib/libzstd.a" ]; then
  cmake -S "zstd-$ZSTD_VER/build/cmake" -B zstd-build \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON \
    -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF \
    $CRT_ARGS \
    || { echo "build_llvm_static.sh: zstd configure failed" >&2; exit 1; }
  cmake --build zstd-build -j"$JOBS" --target install \
    || { echo "build_llvm_static.sh: zstd build failed" >&2; exit 1; }
fi

# LLVM + clang + lld: static archives only, trimmed targets, no z3/xml2.
# ZLIB_ROOT/ZSTD resolve to the just-built statics via CMAKE_PREFIX_PATH;
# the prefix holds no shared libs, so nothing shared can leak in.
# (Unquoted $ZLIB_ARGS/$CMAKE_EXTRA_FLAGS is intentional: multi-flag lists.)
if [ "$WITH_ZLIB" = "FORCE_ON" ]; then
  ZLIB_ARGS="-DLLVM_ENABLE_ZLIB=FORCE_ON -DZLIB_ROOT=$PREFIX"
else
  ZLIB_ARGS="-DLLVM_ENABLE_ZLIB=OFF"
fi
cmake -G Ninja -S "llvm-project-$LLVM_VER.src/llvm" -B llvm-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DBUILD_SHARED_LIBS=OFF \
  -DLLVM_BUILD_LLVM_DYLIB=OFF -DLLVM_LINK_LLVM_DYLIB=OFF \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_TARGETS_TO_BUILD="X86;AArch64" \
  -DLLVM_ENABLE_Z3_SOLVER=OFF \
  -DLLVM_ENABLE_LIBXML2=OFF \
  $ZLIB_ARGS \
  -DLLVM_ENABLE_ZSTD=FORCE_ON -DLLVM_USE_STATIC_ZSTD=ON \
  -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBEDIT=OFF \
  -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_ENABLE_BACKTRACES=OFF \
  -DLLVM_ENABLE_EH=OFF -DLLVM_ENABLE_RTTI=OFF \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_ENABLE_ASSERTIONS=OFF \
  -DLLVM_BUILD_UTILS=OFF \
  $CRT_ARGS \
  ${CMAKE_EXTRA_FLAGS:-} \
  || { echo "build_llvm_static.sh: LLVM configure failed" >&2; exit 1; }
cmake --build llvm-build -j"$JOBS" --target install \
  || { echo "build_llvm_static.sh: LLVM build failed" >&2; exit 1; }

echo "build_llvm_static.sh: static toolchain installed into $PREFIX"
ls "$PREFIX/bin/clang" "$PREFIX/lib/cmake/llvm/LLVMConfig.cmake" \
  || { echo "build_llvm_static.sh: install looks incomplete" >&2; exit 1; }

# Pack the prefix for the stash release; print the sha for llvm_static.pins.
if [ -n "$PACK" ]; then
  command -v zstd >/dev/null 2>&1 || { echo "build_llvm_static.sh: zstd needed for --pack" >&2; exit 1; }
  # Pack from ORIG_DIR so a relative --pack name lands beside the checkout
  # (where llvm-deps.yml's upload step looks), not in the scratch dir.
  ( cd "$ORIG_DIR" && tar -C "$PREFIX" -cf - . | zstd -o "$PACK" ) \
    || { echo "build_llvm_static.sh: pack failed" >&2; exit 1; }
  echo "build_llvm_static.sh: packed $PACK"
  echo "sha256: $(cd "$ORIG_DIR" && SHASUM "$PACK")"
fi
