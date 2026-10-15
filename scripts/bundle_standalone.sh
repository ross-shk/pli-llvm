#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli: a PL/I compiler targeting LLVM
# scripts/bundle_standalone.sh — make a self-contained release directory.
# Usage: bundle_standalone.sh <built-pli-binary> <destdir>
# Result: <destdir>/pli + <destdir>/lib/<shared LLVM closure>.
# Brew/apt ship lld (and brew: z3) shared-only, so a single-file static link
# is impossible from packages; instead the tarball carries the shared libs
# and the binary resolves them via a relative rpath (@loader_path/$ORIGIN),
# keeping the original absolute dirs as fallback (a lone binary still works
# where LLVM is installed). System libc/libm/zlib stay dynamic as usual.
set -u
BIN=$1
DEST=$2

# Windows links statically already (/MT + static LLVM archives): nothing to do.
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*|Windows_NT*) echo "SKIP: windows exe already static"; exit 0 ;;
esac

# Shared libs that belong to the LLVM toolchain (bundled, never system libc).
match_bundle() {
  case "$(basename "$1")" in
    libLLVM*|liblld*|libclang*|libz3*|libzstd*|libxml2*) return 0 ;;
  esac
  return 1
}

mkdir -p "$DEST/lib"
cp "$BIN" "$DEST/pli"
chmod u+w "$DEST/pli"

if [ "$(uname -s)" = "Darwin" ]; then
  # Collect the closure: every referenced path outside the macOS system roots.
  queue="$DEST/pli"
  seen=""
  while [ -n "$queue" ]; do
    cur=${queue%% *}
    if [ "$queue" = "$cur" ]; then queue=""; else queue=${queue#* }; fi
    case " $seen " in *" $cur "*) continue ;; esac
    seen="$seen $cur"
    otool -L "$cur" | tail -n +2 | awk '{print $1}' | while read -r dep; do
      case "$dep" in
        /usr/lib/*|/System/*) ;; # system root: stays dynamic
        @*) ;;                   # already relative: nothing to do
        *) if match_bundle "$dep"; then echo "BUNDLE $dep"; fi ;;
      esac
    done > /tmp/pli_bundle_new.$$
    # Append newly found deps to the work queue.
    while read -r _tag dep; do
      case " $seen " in *" $dep "*) ;; *) queue="$queue $dep" ;; esac
      cp -n "$dep" "$DEST/lib/" 2>/dev/null || true
    done < /tmp/pli_bundle_new.$$
    rm -f /tmp/pli_bundle_new.$$
  done
  # Point every bundled reference at the in-tarball copy. The relative rpath
  # goes first so the bundle is preferred; the absolute toolchain dirs stay
  # as fallback rpaths so a lone binary keeps working where LLVM is installed.
  install_name_tool -delete_rpath "$(otool -l "$DEST/pli" | awk '/LC_RPATH/{f=1} f&&/path /{print $2; exit}')" "$DEST/pli" 2>/dev/null || true
  install_name_tool -add_rpath "@loader_path/lib" "$DEST/pli"
  for f in "$DEST"/lib/*; do
    install_name_tool -id "@rpath/$(basename "$f")" "$f"
    install_name_tool -add_rpath "@loader_path" "$f"
  done
  for f in "$DEST/pli" "$DEST"/lib/*; do
    for dep in $(otool -L "$f" | tail -n +2 | awk '{print $1}'); do
      if match_bundle "$dep" && [ -f "$DEST/lib/$(basename "$dep")" ]; then
        install_name_tool -change "$dep" "@rpath/$(basename "$dep")" "$f"
        install_name_tool -add_rpath "$(dirname "$dep")" "$f" 2>/dev/null || true
      fi
    done
  done
  # install_name_tool invalidates signatures (arm64 kills unsigned dylib
  # loads): re-sign everything ad hoc so the bundle loads anywhere.
  if command -v codesign >/dev/null 2>&1; then
    for f in "$DEST"/lib/* "$DEST/pli"; do
      codesign --force --sign - "$f" 2>/dev/null || true
    done
  fi
  # Backstop: no package-manager absolute path may remain.
  if otool -L "$DEST/pli" "$DEST"/lib/* 2>/dev/null | grep -E "/opt/homebrew|/opt/local|/usr/local/(llvm|lld|zstd|z3)"; then
    echo "FAIL: unbundled package-manager reference remains" >&2; exit 1
  fi
else
  # Linux: resolve the closure through ldd.
  command -v patchelf >/dev/null 2>&1 || \
    { echo "FAIL: patchelf missing (apt install patchelf)" >&2; exit 1; }
  queue="$DEST/pli"
  seen=""
  while [ -n "$queue" ]; do
    cur=${queue%% *}
    if [ "$queue" = "$cur" ]; then queue=""; else queue=${queue#* }; fi
    case " $seen " in *" $cur "*) continue ;; esac
    seen="$seen $cur"
    ldd "$cur" 2>/dev/null | awk '/=>/ {print $1, $3}' | while read -r soname path; do
      if match_bundle "$soname"; then echo "BUNDLE $soname $path"; fi
    done > /tmp/pli_bundle_new.$$
    while read -r _tag soname path; do
      if [ ! -f "$DEST/lib/$soname" ]; then
        cp -L "$path" "$DEST/lib/$soname"
        chmod u+w "$DEST/lib/$soname"
        queue="$queue $DEST/lib/$soname"
      fi
    done < /tmp/pli_bundle_new.$$
    rm -f /tmp/pli_bundle_new.$$
  done
  # Prefer the in-tarball copy, keep the distro dir as fallback for lone use.
  old=$(patchelf --print-rpath "$DEST/pli" 2>/dev/null || true)
  if [ -n "$old" ]; then patchelf --set-rpath "\$ORIGIN/lib:$old" "$DEST/pli"
  else patchelf --set-rpath "\$ORIGIN/lib" "$DEST/pli"; fi
  for f in "$DEST"/lib/*; do
    [ -f "$f" ] || continue
    old=$(patchelf --print-rpath "$f" 2>/dev/null || true)
    if [ -n "$old" ]; then patchelf --set-rpath "\$ORIGIN:$old" "$f"
    else patchelf --set-rpath "\$ORIGIN" "$f"; fi
  done
  # Backstop: every bundled soname must resolve inside the tarball.
  if LD_LIBRARY_PATH="$DEST/lib" ldd "$DEST/pli" | grep -q "not found"; then
    echo "FAIL: bundled binary has unresolved libraries" >&2
    ldd "$DEST/pli" >&2; exit 1
  fi
  if ldd "$DEST/pli" | grep -E "/usr/lib/llvm|/opt/|/usr/local/(llvm|lld)" ; then
    echo "FAIL: unbundled toolchain reference remains" >&2; exit 1
  fi
fi

echo "bundled: $DEST/pli + $(ls "$DEST/lib" 2>/dev/null | wc -l | tr -d ' ') libs"
exit 0
