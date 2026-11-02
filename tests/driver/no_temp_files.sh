#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/no_temp_files.sh — the link path must create zero named
# temporary files. Objects compile into an in-memory buffer and lld reads them
# (plus the embedded sysroot's .o/.a/.tbd blobs) through anonymous-fd paths
# (memfd_create + /proc/self/fd/N on Linux, mkstemp+unlink + /dev/fd/N on
# macOS), so nothing named *.o or pli-sysroot-* may newly appear in the temp
# dir after a link. The only real file produced is the executable (-o).
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=tests/driver/out/no_temp_files
mkdir -p "$OUT"
EXE=
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*|Windows_NT*) EXE=.exe ;;
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

link_and_run() {
  # args: name expected_output <pli inputs...>
  name="$1"; want="$2"; shift 2
  "$PLI" "$@" -o "$OUT/$name$EXE" >"$OUT/$name.link" 2>&1 || { fail "compile+link $name"; cat "$OUT/$name.link"; return 1; }
  after=$(sysroot_count)
  if [ "$after" -ne "$before" ]; then
    fail "sysroot temp leaked after link $name (before=$before after=$after)"
    return 1
  fi
  got=$("$OUT/$name$EXE" 2>/dev/null | tr -d '\r')
  [ "$got" = "$want" ] || { fail "run $name (got: $got, want: $want)"; return 1; }
}

# Snapshot before: my links must not add any sysroot temp dir on top of it.
before=$(sysroot_count)

link_and_run single 'single PASS' "$OUT/single.pli" || true
link_and_run multi 'multi PASS' "$OUT/unit_a.pli" "$OUT/unit_b.pli" || true

# Strict: no sysroot temp dir may remain above the snapshot after the links.
left=$(sysroot_count)
if [ "$left" -ne "$before" ]; then
  fail "sysroot temp dirs remain after links (before=$before after=$left)"
fi

if [ "$ok" -eq 1 ]; then
  echo PASS
  exit 0
else
  exit 1
fi
