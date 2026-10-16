#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/hostcrt.sh — host C-runtime independence (ADR-225): pli links
# against the host CRT from probed filesystem recipes (src/hostcrt.cpp),
# never by asking clang. This test hides every clang from PATH and still
# compiles, links, and runs. (macOS keeps xcrun for the SDK path — Apple
# tooling, not clang; vendored SDK stubs are Phase 4.)
set -u
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
  *) EXE= ;;
esac
PLI=${PLI:-./build/cmake/pli}
OUT=tests/driver/out/hostcrt
mkdir -p tests/driver/out

ok=1

cat > "$OUT.pli" <<'EOF'
 hostcrt: proc options(main);
    put skip list('PASS hostcrt');
 end hostcrt;
EOF

# Shadow clang with a failing stub first on PATH: any exec of clang by pli
# fails the build loudly. (Hiding is not enough — macOS ships /usr/bin/clang
# via the CLT.) The test binary is invoked by explicit path so it still runs.
mkdir -p "$OUT.shadow"
printf '#!/bin/sh\necho "FAIL: shadow clang invoked" >&2\nexit 127\n' > "$OUT.shadow/clang"
chmod +x "$OUT.shadow/clang"

cmd=$(env PATH="$OUT.shadow:/usr/bin:/bin" "$PLI" "$OUT.pli" -o "$OUT$EXE" -v 2>&1) \
  || { echo "FAIL: compile+link with clang shadowed"; ok=0; }
if printf '%s\n' "$cmd" | grep -q '^\+.*clang' ||
   printf '%s\n' "$cmd" | grep -q 'bin/clang'; then
  echo "FAIL: -v transcript invokes clang with clang shadowed on PATH"
  ok=0
fi
if [ "$ok" -eq 1 ]; then
  got=$("$OUT$EXE" | tr -d '\r')
  [ "$got" = "PASS hostcrt" ] || { echo "FAIL: run (got: $got)"; ok=0; }
fi

[ "$ok" -eq 1 ] && echo PASS
exit 0
