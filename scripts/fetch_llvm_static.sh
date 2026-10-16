#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# scripts/fetch_llvm_static.sh — fetch + verify + extract the hermetic static
# LLVM stash (scripts/llvm_static.pins) into a prefix, without compiling.
#
# The stash is a per-platform tarball published on the DEPS_TAG release
# (see .github/workflows/llvm-deps.yml). This is the ONLY script normal CI
# and dev machines run for the toolchain; a source build happens only when a
# pin is bumped (build_llvm_static.sh --pack).
#
# Usage: fetch_llvm_static.sh --prefix <dir>
# Env:   PLI_DEPS_TAG=<tag> override the release tag (testing/forks)
#        PLI_DEPS_URL=<base> override the release base URL
set -u

PREFIX=
while [ "$#" -gt 0 ]; do
  case "$1" in
    --prefix) PREFIX=$2; shift 2 ;;
    *) echo "fetch_llvm_static.sh: unknown arg $1" >&2; exit 2 ;;
  esac
done
[ -n "$PREFIX" ] || { echo "fetch_llvm_static.sh: --prefix required" >&2; exit 2; }

# Resolve this script's dir regardless of cwd.
SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PINS=$SELF_DIR/llvm_static.pins

# Platform name = release matrix key (must match release.yml + llvm-deps.yml).
case "$(uname -s)-$(uname -m)" in
  Linux-x86_64) PLAT=linux-x86_64 ;;
  Linux-aarch64|Linux-arm64) PLAT=linux-aarch64 ;;
  Darwin-arm64|Darwin-aarch64) PLAT=darwin-arm64 ;;
  Darwin-x86_64) PLAT=darwin-x86_64 ;;
  MINGW*|MSYS*|CYGWIN*|Windows_NT*)
    case "$PROCESSOR_ARCHITECTURE" in
      AMD64|x86_64) PLAT=windows-x86_64 ;;
      ARM64) PLAT=windows-arm64 ;;
      *) echo "fetch_llvm_static.sh: unsupported Windows arch" >&2; exit 2 ;;
    esac ;;
  *) echo "fetch_llvm_static.sh: unsupported platform ($(uname -s)-$(uname -m))" >&2; exit 2 ;;
esac

# Read pins. LLVM_VER, DEPS_TAG from the [meta] header; sha per platform row.
LLVM_VER=$(awk -F= '/^LLVM_VER=/{print $2; exit}' "$PINS")
DEPS_TAG=${PLI_DEPS_TAG:-$(awk -F= '/^DEPS_TAG=/{print $2; exit}' "$PINS")}
SHA=$(awk -v p="$PLAT" '$1==p {print $2}' "$PINS")
[ -n "$LLVM_VER" ] && [ -n "$DEPS_TAG" ] || { echo "fetch_llvm_static.sh: pins missing LLVM_VER/DEPS_TAG" >&2; exit 2; }
[ -n "$SHA" ] || { echo "fetch_llvm_static.sh: no stash pin for $PLAT (run llvm-deps promotion first)" >&2; exit 2; }

# GitHub repo (from git remote) so forks/CI resolve the right release.
# Handles both HTTPS (github.com/org/repo.git) and SSH (git@github.com:org/repo.git).
REPO=${PLI_DEPS_REPO:-$(git -C "$SELF_DIR/.." config --get remote.origin.url 2>/dev/null \
  | sed -E 's#(.*github\.com[/:])([^/]+)/([^/.]+)(\.git)?$#\2/\3#')}
[ -n "$REPO" ] || REPO=ross-shk/pli-llvm
BASE=${PLI_DEPS_URL:-https://github.com/$REPO/releases/download/$DEPS_TAG}
TARBALL=llvm-static-$LLVM_VER-$PLAT.tar.zst
URL=$BASE/$TARBALL

# Portable sha256: macOS has shasum -a 256, Linux has sha256sum.
if command -v sha256sum >/dev/null 2>&1; then
  SHASUM() { sha256sum "$1" 2>/dev/null | awk '{print $1}'; }
else
  SHASUM() { shasum -a 256 "$1" 2>/dev/null | awk '{print $1}'; }
fi

# Local cache: one download shared across worktrees.
CACHE_DIR=${XDG_CACHE_HOME:-$HOME/.cache}/pli-deps
CACHE=$CACHE_DIR/$TARBALL
mkdir -p "$CACHE_DIR" "$PREFIX"

if [ -f "$CACHE" ] && [ "$(SHASUM "$CACHE")" = "$SHA" ]; then
  echo "fetch_llvm_static.sh: using cached $TARBALL"
else
  echo "fetch_llvm_static.sh: downloading $URL"
  curl -fL --retry 3 -o "$CACHE" "$URL" || { echo "fetch_llvm_static.sh: download failed" >&2; exit 1; }
fi

# Checksum is the trust anchor: hard fail on mismatch.
GOT=$(SHASUM "$CACHE")
[ "$GOT" = "$SHA" ] || {
  echo "fetch_llvm_static.sh: sha256 mismatch for $TARBALL (got $GOT, want $SHA)" >&2
  echo "stash changed on the server; remove the cached copy and retry, or bump pins" >&2
  rm -f "$CACHE"; exit 1
}

# Extract into the prefix (tarball contains the install-tree contents).
command -v zstd >/dev/null 2>&1 || { echo "fetch_llvm_static.sh: zstd needed to unpack (brew install zstd / apt install zstd)" >&2; exit 1; }
zstd -dc "$CACHE" | tar -C "$PREFIX" -xf -
echo "fetch_llvm_static.sh: extracted $LLVM_VER into $PREFIX"
