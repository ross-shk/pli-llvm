#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/winsock.sh — ADR-281: Windows C-interop links Winsock from the
# vendored sysroot, with no --extra and no -Wl,/DEFAULTLIB:ws2_32. The C bridge
# references WSAStartup/socket/closesocket, so the link only succeeds if
# libws2_32.a is part of the default MinGW link.
set -u
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*|Windows_NT*) EXE=.exe ;;
  *) echo "SKIP: Winsock is Windows-only"; exit 0 ;;
esac
PLI=${PLI:-./build/cmake/pli$EXE}
CLANG=${CLANG:-clang}
OUT=tests/driver/out/winsock
mkdir -p tests/driver/out

cat > "$OUT.c" <<'EOF'
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
int ws_probe(void) {
  WSADATA wsa;
  SOCKET s;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    return -1;
  s = socket(AF_INET, SOCK_STREAM, 0);
  if (s == INVALID_SOCKET) {
    WSACleanup();
    return -2;
  }
  closesocket(s);
  WSACleanup();
  return 0;
}
EOF

cat > "$OUT.pli" <<'EOF'
 winsock: proc options(main);
    declare ws_probe entry returns(fixed bin(31))
       options(linkage(system)) external('ws_probe');
    declare rc fixed bin(31);
    rc = ws_probe();
    if rc = 0 then put skip list('PASS winsock');
    else put skip list('FAIL winsock', rc);
 end winsock;
EOF

"$CLANG" -c "$OUT.c" -o "$OUT.c.o" \
  || { echo "FAIL: C bridge compile"; exit 1; }
"$PLI" -c "$OUT.pli" -o "$OUT.pli.o" \
  || { echo "FAIL: PL/I compile"; exit 1; }
"$PLI" "$OUT.c.o" "$OUT.pli.o" -o "$OUT$EXE" \
  || { echo "FAIL: link without --extra"; exit 1; }
raw=$("$OUT$EXE") || { echo "FAIL: run"; exit 1; }
got=$(printf '%s\n' "$raw" | tr -d '\r')
[ "$got" = "PASS winsock" ] || { echo "FAIL: run (got: $got)"; exit 1; }

echo PASS
