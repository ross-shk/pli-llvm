#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/display_reply.sh — DISPLAY ... REPLY (rule (114)): prints the
# value, then reads one reply line from the active input into the character
# variable (truncated or blank-padded to its length).
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=./tests/driver/out/display_reply
mkdir -p tests/driver/out

cat > "$OUT.pli" <<'EOF'
disp_reply: procedure options(main);
   declare reply_var char(20);
   display('prompt> ') reply(reply_var);
   if reply_var = 'user reply          ' then
      put skip list('PASS display_reply');
   else
      put skip list('FAIL display_reply: got >', reply_var, '<');
end disp_reply;
EOF

$PLI "$OUT.pli" -o "$OUT" || { echo "FAIL display_reply: compile"; exit 1; }
result=$(printf 'user reply\n' | "$OUT")
printf '%s\n' "$result"
printf '%s\n' "$result" | grep -q 'prompt> ' || { echo "FAIL display_reply: prompt missing"; exit 1; }
printf '%s\n' "$result" | grep -q 'PASS display_reply' || { echo "FAIL display_reply: program did not pass"; exit 1; }
exit 0
