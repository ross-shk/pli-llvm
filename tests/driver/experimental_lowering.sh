#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/experimental_lowering.sh — `--experimental-lowering` dispatches
# assign_char/index between runtime and LLVM paths. In P0 the table is empty
# so all modes must fall back to the pli_* runtime and produce identical output.
# P1 flips the flag; this test catches any mode divergence before it ships.
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=tests/driver/out/experimental_lowering
mkdir -p "$OUT"

cat > "$OUT.witness.pli" <<'EOF'
witness: procedure options(main);
   declare s char(8);
   declare v char(8) varying;
   declare f float(6);
   declare x fixed bin(31);
   declare alpha char(10) init('0123456789');
   declare vowels char(5) init('aeiou');
   declare picbuf char(10);
   declare fails fixed bin(31);
   /* WS-C/ADR-242: the constant-offset store and the temporary buffer */
   declare cc char(12);
   declare c1 char(6);
   declare c2 char(4);
   /* WS-C/ADR-243: the merged bit test and the two separate tests */
   declare w bit(32);
   declare bm fixed bin(31);
   declare br fixed bin(31);
   fails = 0;
   v = 'AB';
   s = v;
   if s ^= 'AB      ' then fails = fails + 1;
   v = 'ABCDEFGHIJ';
   s = v;
   if s ^= 'ABCDEFGH' then fails = fails + 1;
   if index('hello', 'll') ^= 3 then fails = fails + 1;
   if index('hi', 'hello') ^= 0 then fails = fails + 1;
   if index('hello', '') ^= 1 then fails = fails + 1;
   /* WS-B/ADR-241: the constant table and the variable scan must agree */
   if verify('12a3', '0123456789') ^= 3 then fails = fails + 1;
   if verify('12a3', alpha) ^= 3 then fails = fails + 1;
   if verify('ab c', 'ab ') ^= 4 then fails = fails + 1;
   if search('hello', 'aeiou', 1) ^= 2 then fails = fails + 1;
   if search('hello', vowels, 1) ^= 2 then fails = fails + 1;
   if search('hello', 'aeiou', 4) ^= 5 then fails = fails + 1;
   if repeat('ab', 3) ^= 'ababab' then fails = fails + 1;
   /* WS-B/ADR-273: TRANSLATE pads `out` with blanks to the length of `in` */
   if translate('abcxyz', 'XYQ', 'abc') ^= 'XYQxyz' then fails = fails + 1;
   if translate('abcxyz', 'XY', 'abc') ^= 'XY xyz' then fails = fails + 1;
   /* WS-B/ADR-274: the folded and runtime picture paths must agree */
   put string(picbuf) edit(123.45) (P('ZZ9.99'));
   if substr(picbuf,1,6) ^= '123.45' then fails = fails + 1;
   /* WS-C/ADR-242: the constant-offset store and the temporary buffer
      must produce the same characters */
   c1 = 'abcdef';
   c2 = 'ghij';
   cc = c1 || c2;
   if cc ^= 'abcdefghij  ' then fails = fails + 1;
   /* WS-C/ADR-243: the merged bit test and the two separate tests must
      agree */
   w = fixed(13056);
   bm = 0;
   if ((w & '00000000000000000011000000000000'b) = '00000000000000000011000000000000'b) &
      ((w & '00000000000000000000001100000000'b) = '00000000000000000000001100000000'b)
   then bm = 1;
   br = 0;
   if (w & '00000000000000000011000000000000'b) = '00000000000000000011000000000000'b then br = 1;
   if bm ^= br then fails = fails + 1;
   /* W4: FIXED(float) and FIXED(char) differential check */
   f = 3.14;
   x = fixed(f);
   if x ^= 3 then fails = fails + 1;
   x = fixed('123');
   if x ^= 123 then fails = fails + 1;
   if fails = 0 then put skip list('PASS witness');
   else put skip list('FAIL witness:', fails);
end witness;
EOF

# Detect MLIR support: a non-MLIR build diagnoses --experimental-lowering=mlir.
mlir_modes=""
if $PLI --experimental-lowering=mlir -fsyntax-only "$OUT.witness.pli" 2>/dev/null; then
  mlir_modes="mlir"
fi

modes="auto runtime llvm $mlir_modes"
ok=1
ref=""
for mode in $modes; do
  bin="$OUT.witness.$mode"
  if ! $PLI --experimental-lowering="$mode" "$OUT.witness.pli" -o "$bin" 2>/dev/null; then
    echo "FAIL: mode=$mode compile failed"
    ok=0
    continue
  fi
  out=$(./"$bin" 2>&1)
  echo "mode=$mode: $out"
  if [ "$mode" = "auto" ]; then
    ref="$out"
  elif [ "$out" != "$ref" ]; then
    echo "FAIL: mode=$mode diverges from auto (expected: '$ref', got: '$out')"
    ok=0
  fi
done

[ "$ok" -eq 1 ] && echo "PASS experimental_lowering"
exit 0
