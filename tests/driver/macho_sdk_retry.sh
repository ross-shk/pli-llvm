#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/macho_sdk_retry.sh — ADR-280: a MachO program referencing a
# symbol outside the vendored libSystem stub still links, by retrying with the
# host SDK's libSystem.B.tbd. The symbol here is getuid(), absent from
# platform/darwin-sdk/usr/lib/libSystem.tbd. No --extra, no xcrun.
set -u
case "$(uname -s)" in
  Darwin*) ;;
  *) echo "SKIP: not macOS"; exit 0 ;;
esac
PLI=${PLI:-./build/cmake/pli}
OUT=tests/driver/out/macho_sdk_retry
mkdir -p tests/driver/out

# Probe the same SDK locations as the driver (findMacOSSdk); no host SDK means
# the old undefined-symbol failure is the only possible outcome.
SDK=
for root in /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk \
            /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk; do
  if [ -f "$root/usr/lib/libSystem.B.tbd" ]; then
    SDK=$root
    break
  fi
done
[ -n "$SDK" ] || { echo "SKIP: no host macOS SDK"; exit 0; }

cat > "$OUT.pli" <<'EOF'
 macho_retry: proc options(main);
    declare getuid entry returns(fixed bin(31)) external('getuid');
    declare u fixed bin(31);
    u = getuid();
    if u >= 0 then put skip list('PASS macho_retry');
    else put skip list('FAIL macho_retry', u);
 end macho_retry;
EOF

cmd=$("$PLI" "$OUT.pli" -o "$OUT" -v 2>&1) \
  || { echo "FAIL: link requiring a symbol outside the vendored stub"; printf '%s\n' "$cmd"; exit 1; }
printf '%s\n' "$cmd" | grep -q 'retrying MachO link with host SDK' \
  || { echo "FAIL: host SDK retry not reported"; exit 1; }
got=$("$OUT") || { echo "FAIL: retry-linked binary did not run"; exit 1; }
[ "$got" = "PASS macho_retry" ] || { echo "FAIL: run (got: $got)"; exit 1; }

echo PASS
