#!/bin/sh
# tests/driver/get_complex.sh — GET LIST of complex (CM5): a `re+imI`
# token reads both parts, a bare number reads with zero imaginary part.
# The program verifies the values itself and prints PASS.
set -u
PLIC=./build/plic
OUT=./tests/driver/out/get_complex
mkdir -p tests/driver/out

cat > "$OUT.pli" <<'EOF'
 get_complex: procedure options(main);
    declare (a, b, c) complex;
    declare fails fixed bin(31);
    fails = 0;
    get list (a, b, c);
    if real(a) ^= 4.0 | imag(a) ^= 6.0 then do;
       fails = fails + 1;
       put skip list('FAIL a');
    end;
    if real(b) ^= -5.0 | imag(b) ^= 10.0 then do;
       fails = fails + 1;
       put skip list('FAIL b');
    end;
    if real(c) ^= 7.0 | imag(c) ^= 0.0 then do;
       fails = fails + 1;
       put skip list('FAIL c');
    end;
    if fails = 0 then put skip list('PASS get-complex');
    else put skip list('FAIL get-complex');
 end get_complex;
EOF

$PLIC "$OUT.pli" -o "$OUT" || { echo "FAIL get-complex: compile"; exit 1; }
printf '4+6I -5+10I 7\n' | "$OUT"
exit 0
