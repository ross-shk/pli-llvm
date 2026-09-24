#!/bin/sh
# tests/driver/sysparm.sh — SYSPARM builtin (rule (123)): sysparm()
# returns the --sysparm option value baked in at compile time (empty when
# none is given). Compiles one program with --sysparm HELLO and one without.
set -u
PLIC=./build/plic
OUT=./tests/driver/out/sysparm
mkdir -p tests/driver/out

cat > "$OUT.pli" <<'EOF'
 sysparm_flag: procedure options(main);
    declare s char(64) varying;
    declare fails fixed bin(31);
    fails = 0;
    s = sysparm();
    if s ^= 'HELLO' then do;
       fails = fails + 1;
       put skip list('FAIL s =', s);
    end;
    if length(sysparm()) ^= 5 then do;
       fails = fails + 1;
       put skip list('FAIL length =', length(sysparm()));
    end;
    if fails = 0 then put skip list('PASS sysparm flag');
    else put skip list('FAIL sysparm flag: fails =', fails);
 end sysparm_flag;
EOF

cat > "$OUT.empty.pli" <<'EOF'
 sysparm_empty: procedure options(main);
    declare fails fixed bin(31);
    fails = 0;
    if length(sysparm()) ^= 0 then do;
       fails = fails + 1;
       put skip list('FAIL length =', length(sysparm()));
    end;
    if fails = 0 then put skip list('PASS sysparm empty');
    else put skip list('FAIL sysparm empty: fails =', fails);
 end sysparm_empty;
EOF

$PLIC --sysparm HELLO "$OUT.pli" -o "$OUT" || { echo "FAIL sysparm: compile with flag"; exit 1; }
( cd tests/driver/out && ./sysparm ) || exit 1
$PLIC "$OUT.empty.pli" -o "$OUT.empty" || { echo "FAIL sysparm: compile default"; exit 1; }
( cd tests/driver/out && ./sysparm.empty ) || exit 1
exit 0
