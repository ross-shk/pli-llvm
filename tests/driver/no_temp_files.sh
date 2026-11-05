#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/no_temp_files.sh — the link path must create zero named
# temporary files. Objects compile into an in-memory buffer and lld reads them
# (plus the embedded sysroot's .o/.a/.tbd blobs) through anonymous-fd paths
# (memfd_create + /proc/self/fd/N on Linux, mkstemp+unlink + /dev/fd/N on
# macOS), so nothing named *.o, pli-mem-* or pli-sysroot-* may newly appear in
# the temp dir after a link. Windows has no anonymous-fd lld can reopen, so
# mem blobs fall back to uniquely named temp files (same pli-mem-* marker) that
# closeMemBlob must delete; this script's count-diff catches a leaked one. The
# only real file produced is the executable (-o).
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=tests/driver/out/no_temp_files
mkdir -p "$OUT"
EXE=
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*|Windows_NT*) EXE=.exe ;;
esac
# Windows has no anonymous-fd support (no memfd_create, no /proc/self/fd/N),
# so sysroot is materialized to disk and mem blobs use named temp files that
# Windows file-locking may prevent from being deleted. This test's zero-temp
# assumption is Linux/macOS-specific; skip on Windows.
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*|Windows_NT*)
    echo "PASS (skipped on Windows: no anonymous-fd support)"
    exit 0
    ;;
esac
TDIR="${TMPDIR:-/tmp}"

cat > "$OUT/single.pli" <<'EOF'
  single: proc options(main);
     put skip list('single PASS');
  end single;
EOF
cat > "$OUT/unit_a.pli" <<'EOF'
  unit_a: proc;
     put skip list('unit A PASS');
  end unit_a;
EOF
cat > "$OUT/unit_b.pli" <<'EOF'
  unit_b: proc options(main);
     put skip list('multi PASS');
  end unit_b;
EOF

ok=1
fail() { echo "FAIL: $1"; ok=0; }

# Count pli-sysroot-* temp directories directly under the temp dir. Only the
# (now removed) disk sysroot materialization created these; a link that keeps
# the sysroot in memory must not change the count.
sysroot_count() {
  find "$TDIR" -maxdepth 1 -name 'pli-sysroot-*' 2>/dev/null | wc -l
}

# Count pli-mem-* blob temp files directly under the temp dir. The Linux/macOS
# blobs are anonymous (never a named file), but on Windows each blob is a
# uniquely named temp file that closeMemBlob must remove; a link that leaks one
# raises this count.
mem_count() {
  find "$TDIR" -maxdepth 1 -type f -name 'pli-mem-*' 2>/dev/null | wc -l
}

link_and_run() {
  # args: name expected_output <pli inputs...>
  name="$1"; want="$2"; shift 2
  "$PLI" "$@" -o "$OUT/$name$EXE" >"$OUT/$name.link" 2>&1 || { fail "compile+link $name"; cat "$OUT/$name.link"; return 1; }
  after=$(sysroot_count)
  if [ "$after" -ne "$before" ]; then
    fail "sysroot temp leaked after link $name (before=$before after=$after)"
    return 1
  fi
  after_mem=$(mem_count)
  if [ "$after_mem" -ne "$before_mem" ]; then
    fail "mem blob temp leaked after link $name (before=$before_mem after=$after_mem)"
    return 1
  fi
  got=$("$OUT/$name$EXE" 2>/dev/null | tr -d '\r')
  [ "$got" = "$want" ] || { fail "run $name (got: $got, want: $want)"; return 1; }
}

# Snapshot before: my links must not add any sysroot temp dir or mem blob temp
# file on top of it.
before=$(sysroot_count)
before_mem=$(mem_count)

link_and_run single 'single PASS' "$OUT/single.pli" || true
link_and_run multi 'multi PASS' "$OUT/unit_a.pli" "$OUT/unit_b.pli" || true

# Strict: no sysroot temp dir or mem blob temp file may remain above the
# snapshot after the links.
left=$(sysroot_count)
if [ "$left" -ne "$before" ]; then
  fail "sysroot temp dirs remain after links (before=$before after=$left)"
fi
left_mem=$(mem_count)
if [ "$left_mem" -ne "$before_mem" ]; then
  fail "mem blob temp files remain after links (before=$before_mem after=$left_mem)"
fi

if [ "$ok" -eq 1 ]; then
  echo PASS
  exit 0
else
  exit 1
fi
