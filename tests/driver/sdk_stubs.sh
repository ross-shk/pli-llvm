#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/sdk_stubs.sh — vendored macOS SDK stubs (ADR-226): a pli with
# an exe-relative sdk/ dir links without xcrun/Xcode (macOS only; SKIP
# elsewhere). Copies pli to a temp dir so the build tree stays clean.
set -u
case "$(uname -s)" in
  Darwin) ;;
  *) echo "SKIP: macOS-only SDK stubs"; exit 0 ;;
esac
PLI=${PLI:-./build/cmake/pli}
OUT=tests/driver/out/sdk_stubs
mkdir -p tests/driver/out

ok=1
cat > "$OUT.pli" <<'EOF'
 stub: proc options(main);
    declare x fixed bin(31);
    declare y float bin(53);
    x = 21;
    y = x * 2.0;
    put skip list('PASS sdk_stubs', x, y);
 end stub;
EOF

# Pristine stage: pli + sdk/ only (no CLT in the picture — PATH scrubbed of
# xcrun would break nothing since the stub dir wins; assert it wins via -v).
rm -rf "$OUT.stage"
mkdir -p "$OUT.stage"
cp "$PLI" "$OUT.stage/pli" || { echo "FAIL: stage pli copy"; ok=0; }
cp -r platform/darwin-sdk "$OUT.stage/sdk" || { echo "FAIL: stage sdk copy"; ok=0; }
if [ "$ok" -eq 1 ]; then
  cmd=$("$OUT.stage/pli" "$OUT.pli" -o "$OUT.bin" -v 2>&1) \
    || { echo "FAIL: stub-sdk compile+link"; ok=0; }
  printf '%s\n' "$cmd" | grep -q 'vendored SDK stubs' \
    || { echo "FAIL: stub sdk dir not picked up"; ok=0; }
  got=$("$OUT.bin" 2>/dev/null | tr -d '\r')
  [ "$got" = "PASS sdk_stubs 21 42" ] || { echo "FAIL: stub run (got: $got)"; ok=0; }
  # No Apple SDK path may leak into the link when stubs are present.
  if printf '%s\n' "$cmd" | grep -q 'MacOSX.sdk\|Xcode'; then
    echo "FAIL: real SDK referenced despite stubs"
    ok=0
  fi
fi

[ "$ok" -eq 1 ] && echo PASS
exit 0
