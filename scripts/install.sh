#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# scripts/install.sh - one-line Unix install for the pli compiler (Linux + macOS).
#
# Usual usage (fetched from the latest release and executed):
#   curl -fsSL https://github.com/ross-shk/pli-llvm/releases/latest/download/install.sh | sh
#
# Or directly:
#   sh scripts/install.sh [--version <tag>] [--dir <path>]
#
# What it does:
#   1. Detects the host OS + architecture and maps it to a release asset
#      (pli-linux-x86_64, pli-linux-aarch64, or pli-darwin-arm64).
#   2. Downloads the <asset>.tar.gz and verifies its sha256 against the
#      published <asset>.tar.gz.sha256 unless --skip-verify (verification
#      requires the per-tag checksum asset shipped by the release workflow;
#      if it is missing the install fails fast and points at --skip-verify).
#   3. Extracts the single stripped 'pli' binary into ~/.pli/bin (or --dir).
#   4. Appends the bin dir to PATH via the login/shell rc files (~/.profile,
#      ~/.bashrc, ~/.zshrc) only when not already present (idempotent, no
#      duplicate exports). Use --no-path to skip.
#
# Pure POSIX sh (no bashisms): may run as /bin/sh on both Linux and macOS.
# NOTE: keep this file pure ASCII.

set -u

REPO="ross-shk/pli-llvm"
BASE_URL="https://github.com/$REPO/releases"
API_URL="https://api.github.com/repos/$REPO/releases/latest"

VERSION=""
DEST_DIR=""
SKIP_VERIFY=0
NO_PATH=0

# Shell rc/login files used to add the bin dir to PATH (set here so the final
# helper message can reference them even with --no-path).
PROFILE="${HOME}/.profile"
case "${SHELL:-}" in
  *zsh)  RC_FILE="${ZDOTDIR:-$HOME}/.zshrc" ;;
  *bash) RC_FILE="${HOME}/.bashrc" ;;
  *)     RC_FILE="" ;;
esac

usage() {
  cat <<'EOF'
Usage: install.sh [options]

  --version <tag>   install a specific release tag (default: latest)
  --dir <path>      install directory (default: ~/.pli/bin)
  --skip-verify     do not check the sha256 of the download
  --no-path         do not modify shell rc files / PATH
  --help            show this help
EOF
  exit 0
}

# --- Argument parsing -------------------------------------------------------
# Support both "--flag value" and "--flag=value". Reads the next argument
# safely so an option at the end (no value) degrades gracefully instead of
# aborting on an unset variable under `set -u`.
need_value() {
  if [ $# -lt 2 ]; then
    echo "install.sh: option '$1' requires a value" >&2
    exit 1
  fi
}

while [ $# -gt 0 ]; do
  case "$1" in
    --version=*)       VERSION="${1#--version=}"; shift ;;
    --version)         need_value "$@"; VERSION="$2"; shift 2 ;;
    --dir=*)           DEST_DIR="${1#--dir=}"; shift ;;
    --dir)             need_value "$@"; DEST_DIR="$2"; shift 2 ;;
    --skip-verify)     SKIP_VERIFY=1; shift ;;
    --no-path)         NO_PATH=1; shift ;;
    --help|-h)         usage ;;
    *) echo "install.sh: unknown option '$1'" >&2; usage ;;
  esac
done

# --- Detect host OS + arch -> asset -----------------------------------------
OS="$(uname -s)"
if [ "$OS" = "Linux" ]; then
  ARCH="$(uname -m)"
  case "$ARCH" in
    x86_64|amd64) ASSET="pli-linux-x86_64" ;;
    aarch64|arm64) ASSET="pli-linux-aarch64" ;;
    *)
      echo "install.sh: unsupported Linux architecture '$ARCH' (only x86_64 and aarch64 releases are published)" >&2
      exit 1 ;;
  esac
elif [ "$OS" = "Darwin" ]; then
  ARCH="$(uname -m)"
  case "$ARCH" in
    arm64) ASSET="pli-darwin-arm64" ;;
    *)
      echo "install.sh: unsupported macOS architecture '$ARCH' (only Apple Silicon / arm64 releases are published)" >&2
      exit 1 ;;
  esac
else
  echo "install.sh: unsupported OS '$OS' (supported: Linux, macOS)" >&2
  exit 1
fi

# --- Resolve tag ------------------------------------------------------------
resolve_tag() {
  # Try the GitHub API for the latest tag. Prefer curl, fall back to wget.
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL -H 'User-Agent: pli-installer' "$API_URL" 2>/dev/null \
      | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1
  elif command -v wget >/dev/null 2>&1; then
    wget -qO- --header='User-Agent: pli-installer' "$API_URL" 2>/dev/null \
      | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1
  else
    echo ""
  fi
}

if [ -n "$VERSION" ]; then
  TAG="$VERSION"
else
  echo "install.sh: resolving latest release tag ..."
  TAG="$(resolve_tag)"
  if [ -z "$TAG" ]; then
    # API unavailable. Fall back to GitHub's latest/download redirects for both
    # the tarball and its checksum (works without knowing the tag up front).
    echo "install.sh: failed to resolve latest release; using latest/download URLs" >&2
  else
    echo "install.sh: release tag = $TAG"
  fi
fi

# Build URLs; when $TAG is empty we use GitHub's latest/download redirect.
TARBALL="$ASSET.tar.gz"
if [ -n "$TAG" ]; then
  URL="$BASE_URL/download/$TAG/$TARBALL"
  SHA_URL="$BASE_URL/download/$TAG/$TARBALL.sha256"
else
  URL="$BASE_URL/latest/download/$TARBALL"
  SHA_URL="$BASE_URL/latest/download/$TARBALL.sha256"
fi

TMPDIR="${TMPDIR:-/tmp}/pli-install-$$"
mkdir -p "$TMPDIR"
trap 'rm -rf "$TMPDIR"' EXIT INT TERM

# --- Download ---------------------------------------------------------------
# fetch_to <url> <outfile>: prefer curl, fall back to wget. Returns non-zero on
# failure (curl -f / wget non-zero) so callers can detect a missing asset.
fetch_to() {
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL --retry 3 -o "$2" "$1"
  elif command -v wget >/dev/null 2>&1; then
    wget -q -O "$2" "$1"
  else
    echo "install.sh: neither curl nor wget found" >&2
    return 1
  fi
}

echo "install.sh: downloading $URL"
if ! fetch_to "$URL" "$TMPDIR/$TARBALL"; then
  echo "install.sh: download failed" >&2
  exit 1
fi
[ -s "$TMPDIR/$TARBALL" ] || { echo "install.sh: download failed or empty" >&2; exit 1; }

# --- Verify sha256 ----------------------------------------------------------
if [ "$SKIP_VERIFY" -eq 0 ]; then
  echo "install.sh: verifying sha256 ..."
  if command -v sha256sum >/dev/null 2>&1; then
    SHA="sha256sum"
  elif command -v shasum >/dev/null 2>&1; then
    SHA="shasum"
  else
    echo "install.sh: no sha256sum/shasum available; re-run with --skip-verify to bypass" >&2
    exit 1
  fi
  if ! fetch_to "$SHA_URL" "$TMPDIR/$TARBALL.sha256" 2>/dev/null; then
    echo "install.sh: could not fetch checksum ($SHA_URL). Re-run with --skip-verify to bypass integrity checking." >&2
    exit 1
  fi
  EXPECTED="$(sed -n 's/[[:space:]].*//p' "$TMPDIR/$TARBALL.sha256" | tr -d '\r' | head -n1)"
  if [ -z "$EXPECTED" ]; then
    echo "install.sh: checksum file is empty ($SHA_URL). Re-run with --skip-verify to bypass integrity checking." >&2
    exit 1
  fi
  if [ "$SHA" = "shasum" ]; then
    ACTUAL="$(cd "$TMPDIR" && shasum -a 256 "$TARBALL" | sed -n 's/[[:space:]].*//p')"
  else
    ACTUAL="$(cd "$TMPDIR" && sha256sum "$TARBALL" | sed -n 's/[[:space:]].*//p')"
  fi
  if [ "$EXPECTED" != "$ACTUAL" ]; then
    echo "install.sh: sha256 mismatch. Expected $EXPECTED, got $ACTUAL. The download is corrupt or tampered with." >&2
    exit 1
  fi
  echo "install.sh: sha256 OK"
fi

# --- Extract + install ------------------------------------------------------
if [ -z "$DEST_DIR" ]; then
  DEST_DIR="$HOME/.pli/bin"
fi
mkdir -p "$DEST_DIR"
# The tarball contains the binary at <platdir>/pli (single file). Find it by
# name so we never depend on the exact top-level directory name.
tar -xzf "$TMPDIR/$TARBALL" -C "$TMPDIR"
FOUND="$(find "$TMPDIR" -type f -name pli | head -n1)"
if [ -z "$FOUND" ]; then
  echo "install.sh: pli binary not found inside the archive" >&2
  exit 1
fi
cp "$FOUND" "$DEST_DIR/pli"
chmod +x "$DEST_DIR/pli"
echo "install.sh: installed $DEST_DIR/pli"

# --- PATH via shell rc files (idempotent) ------------------------------------
# Appends an `export PATH` line to the rc file(s) the user's shell reads, but
# only when no `# >>> pli >>>` block already exists, so re-runs never duplicate.
if [ "$NO_PATH" -eq 0 ]; then
  # Append to the interactive shell's rc file (if any) and always ~/.profile so
  # login shells (sh, bash, zsh) pick up the dir.
  for F in "$PROFILE" "$RC_FILE"; do
    [ -n "$F" ] || continue
    if [ -f "$F" ] && grep -q '# >>> pli >>>' "$F" 2>/dev/null; then
      echo "install.sh: $F already has pli on PATH"
      continue
    fi
    {
      echo ""
      echo "# >>> pli >>>"
      echo "export PATH=\"$DEST_DIR:\$PATH\""
      echo "# <<< pli <<<"
    } >> "$F"
    echo "install.sh: added $DEST_DIR to PATH in $F"
  done
else
  echo "install.sh: skipped PATH changes (--no-path)"
fi

# --- Confirm ----------------------------------------------------------------
"$DEST_DIR/pli" --version
if [ $? -ne 0 ]; then
  echo "install.sh: installed binary failed its --version check" >&2
  exit 1
fi

echo ""
echo "pli installed to: $DEST_DIR/pli"
echo "Open a NEW shell (or 'source $PROFILE${RC_FILE:+ / $RC_FILE}') so $DEST_DIR is on PATH, then run: pli --help"
exit 0
