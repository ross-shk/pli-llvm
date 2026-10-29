#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/mm_three_way.sh — three separately compiled units (rules
# (42),(91)-(94)): MAIN calls package B, B recurses through a private
# procedure that arms an ON-unit per level, calls package C, which
# signals a named condition, and calls back into MAIN. Each level's unit
# must catch C's signal from its own call, and be gone when it returns.
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=tests/driver/out/mm_three_way
mkdir -p tests/driver/out
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
  *) EXE= ;;
esac

cat > "$OUT.main.pli" <<'EOF'
 /* SPDX-License-Identifier: Apache-2.0 */
 mm_three_way: procedure options(main);
    declare calls fixed bin(31) external;
    declare noted fixed bin(31) external;
    declare evt_hits fixed bin(31) external;
    declare evt_codes(4) fixed bin(31) external;
    declare entry_b entry(fixed bin(31)) returns(fixed bin(31))
       external;
    declare n fixed bin(31);
    declare r fixed bin(31);
    declare i fixed bin(31);
    calls = 0;
    noted = 0;
    evt_hits = 0;
    do i = 1 to 4;
       evt_codes(i) = -1;
    end;
    n = 3;
    r = entry_b(n);
    if r ^= 60 | calls ^= 4 | noted ^= 4 | evt_hits ^= 4 then do;
       put skip list('FAIL mm_three_way:', r, calls, noted,
          evt_hits);
       stop;
    end;
    do i = 1 to 4;
       if evt_codes(i) ^= 99 + i then do;
          put skip list('FAIL mm_three_way: code', i, evt_codes(i));
          stop;
       end;
    end;
    put skip list('PASS mm_three_way');
 end mm_three_way;
 note: procedure(k);
    declare k fixed bin(31);
    declare noted fixed bin(31) external;
    noted = noted + 1;
 end note;
EOF

cat > "$OUT.b.pli" <<'EOF'
 /* SPDX-License-Identifier: Apache-2.0 */
 mm3_b: package exports(entry_b);
 declare calls fixed bin(31) external;
 declare evt_hits fixed bin(31) external;
 declare evt_codes(4) fixed bin(31) external;
 declare evt condition;
 declare entry_c entry(fixed bin(31)) returns(fixed bin(31))
    external;
 declare note entry(fixed bin(31)) external;
 entry_b: procedure(n) returns(fixed bin(31));
    declare n fixed bin(31);
    return(bstep(n));
 end entry_b;
 bstep: procedure(n) returns(fixed bin(31)) recursive;
    declare n fixed bin(31);
    declare code fixed bin(31);
    declare sub fixed bin(31);
    declare m fixed bin(31);
    code = n + 100;
    /* the slot is this level's code; the value is C's ONCODE */
    on condition(evt) begin;
       evt_hits = evt_hits + 1;
       evt_codes(code - 99) = oncode();
    end;
    sub = 0;
    if n > 0 then do;
       m = n - 1;
       sub = bstep(m);
    end;
    calls = calls + 1;
    sub = sub + entry_c(n);
    call note(n);
    return(sub);
 end bstep;
 end mm3_b;
EOF

cat > "$OUT.c.pli" <<'EOF'
 /* SPDX-License-Identifier: Apache-2.0 */
 mm3_c: package exports(entry_c);
 entry_c: procedure(n) returns(fixed bin(31));
    declare n fixed bin(31);
    declare evt condition;
    signal condition(evt) set oncode(n + 100);
    return(n * 10);
 end entry_c;
 end mm3_c;
EOF

for u in main b c; do
  $PLI "$OUT.$u.pli" -c -o "$OUT.$u.o" || { echo "FAIL mm_three_way: compile $u"; exit 1; }
done
$PLI "$OUT.main.o" "$OUT.b.o" "$OUT.c.o" -o "$OUT$EXE" || { echo "FAIL mm_three_way: link"; exit 1; }
out=$("./$OUT$EXE" 2>&1)
status=$?
echo "$out"
if [ $status -ne 0 ]; then
  echo "FAIL mm_three_way: exit $status"
  exit 1
fi
echo "$out" | grep -q 'PASS mm_three_way' || { echo "FAIL mm_three_way: no PASS"; exit 1; }
exit 0
