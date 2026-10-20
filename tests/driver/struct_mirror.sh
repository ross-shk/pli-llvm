#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/struct_mirror.sh — the struct-mirror gate (ADR-261): a PL/I
# structure and its C twin must agree on layout, member by member.
#
# Three assertions:
#   1. Layout  — the LLVM type of the PL/I structure equals the C structure's
#                type, so size and every member offset are identical.
#   2. Interop — C writes the record at its own offsets and PL/I reads the
#                members back (and the other way round), so a layout change
#                shows up as a wrong value, not as a matching IR string.
#   3. Vectorization — the -O2 loop over the structure vectorizes in the PL/I
#                build exactly when it vectorizes in the C build.
#
# The mirrored structure deliberately needs padding (a byte between two
# words), so the gate exercises the padding rule rather than a trivial
# multiple of the member size.
set -u
PLI=${PLI:-./build/cmake/pli}
CLANG=${CLANG:-clang}
OUT=tests/driver/out/struct_mirror
mkdir -p "$OUT"

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
  *) EXE= ;;
esac

fail() { echo "FAIL struct-mirror: $1"; exit 1; }

# --- the PL/I program -------------------------------------------------------
cat > "$OUT.pli" <<'EOF'
 struct_mirror: procedure options(main);
   dcl 1 rec,
      2 tag  fixed bin(31),
      2 flag bit(1),
      2 code fixed bin(31),
      2 name char(8);
   dcl 1 recs(512),
      2 tag  fixed bin(31),
      2 flag bit(1),
      2 code fixed bin(31),
      2 name char(8);
   dcl 1 pairs(512),
      2 a fixed bin(31),
      2 b fixed bin(31);
   dcl c_size  entry returns(fixed bin(31)) options(linkage(system)) external('sm_c_size');
   dcl c_write entry (pointer) options(linkage(system)) external('sm_c_write');
   dcl c_read_tag  entry (pointer) returns(fixed bin(31)) options(linkage(system)) external('sm_c_read_tag');
   dcl c_read_flag entry (pointer) returns(fixed bin(31)) options(linkage(system)) external('sm_c_read_flag');
   dcl c_read_code entry (pointer) returns(fixed bin(31)) options(linkage(system)) external('sm_c_read_code');
   dcl fails fixed bin(31) init(0);
   dcl i fixed bin(31);
   dcl sum fixed bin(63) init(0);

   /* C writes every member at its own offsets; PL/I reads them at its own. */
   call c_write(addr(rec));
   if rec.tag ^= 111 then fails = fails + 1;
   if rec.flag ^= '1'b then fails = fails + 1;
   if rec.code ^= 222 then fails = fails + 1;
   if rec.name ^= 'abcdefgh' then fails = fails + 1;

   /* PL/I writes the members; C reads them back at its own offsets. */
   rec.tag = 333;
   rec.flag = '0'b;
   rec.code = 444;
   if c_read_tag(addr(rec)) ^= 333 then fails = fails + 1;
   if c_read_flag(addr(rec)) ^= 0 then fails = fails + 1;
   if c_read_code(addr(rec)) ^= 444 then fails = fails + 1;

   /* The C size is the size the PL/I layout must produce. */
   if c_size() ^= 20 then fails = fails + 1;

   /* The loops the vectorization assertion compares. */
   do i = 1 to 512;
      recs(i).tag = i;
      recs(i).code = i * 2;
   end;
   do i = 1 to 512;
      sum = sum + recs(i).tag + recs(i).code;
   end;
   do i = 1 to 512;
      pairs(i).a = i;
      pairs(i).b = i * 2;
   end;
   do i = 1 to 512;
      sum = sum + pairs(i).a + pairs(i).b;
   end;

   put skip list('checksum', sum);
   if fails = 0 then put skip list('PASS struct_mirror');
   else put skip list('FAIL struct_mirror: fails =', fails);
 end struct_mirror;
EOF

# --- the C twin -------------------------------------------------------------
cat > "$OUT.c" <<'EOF'
/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
 * The C twin of the PL/I structure: the same members, so both layouts must
 * agree byte for byte. Built twice: with SM_STANDALONE it is the standalone
 * program the vectorization assertion compares against. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct sm_rec {   /* padded: the byte member leaves a hole before code */
  int32_t tag;
  unsigned char flag;
  int32_t code;
  char name[8];
};

struct sm_pair {  /* compact: two words, no padding */
  int32_t a;
  int32_t b;
};

#ifdef SM_STANDALONE
int main(void) {
  struct sm_rec recs[512];
  struct sm_pair pairs[512];
  long long sum = 0;
  for (int i = 0; i < 512; i++) {
    recs[i].tag = i + 1;
    recs[i].code = (i + 1) * 2;
  }
  for (int i = 0; i < 512; i++)
    sum += recs[i].tag + recs[i].code;
  for (int i = 0; i < 512; i++) {
    pairs[i].a = i + 1;
    pairs[i].b = (i + 1) * 2;
  }
  for (int i = 0; i < 512; i++)
    sum += pairs[i].a + pairs[i].b;
  printf("checksum %lld\n", sum);
  return 0;
}
#else
int sm_c_size(void) { return (int)sizeof(struct sm_rec); }

void sm_c_write(struct sm_rec *r) {
  r->tag = 111;
  r->flag = 1;
  r->code = 222;
  r->name[0] = 'a'; r->name[1] = 'b'; r->name[2] = 'c'; r->name[3] = 'd';
  r->name[4] = 'e'; r->name[5] = 'f'; r->name[6] = 'g'; r->name[7] = 'h';
}

int sm_c_read_tag(struct sm_rec *r) { return r->tag; }
int sm_c_read_flag(struct sm_rec *r) { return r->flag; }
int sm_c_read_code(struct sm_rec *r) { return r->code; }
#endif
EOF

# --- 1. the layout: the LLVM types must be the same -------------------------
$PLI -emit-llvm "$OUT.pli" -o "$OUT.pli.ll" 2>"$OUT.pli.err" || fail "PL/I IR emission failed"
$CLANG -O0 -Xclang -disable-O0-optnone -emit-llvm -S "$OUT.c" -o "$OUT.c.ll" 2>"$OUT.c.err" || fail "C IR emission failed"

# The C type body, e.g. "{ i32, i8, i32, [8 x i8] }".
ctype=$(sed -n 's/^%struct\.sm_rec = type \(.*\)$/\1/p' "$OUT.c.ll" | head -1)
[ -n "$ctype" ] || fail "no C struct type in the C IR"
grep -qF "$ctype" "$OUT.pli.ll" || fail "PL/I layout differs from C: C is $ctype"

# --- 2. the interop: build both units, link them, run them -----------------
$PLI "$OUT.pli" -c -o "$OUT.pli.o" 2>"$OUT.pli.err" || fail "PL/I compile failed"
$CLANG -c "$OUT.c" -o "$OUT.c.o" 2>"$OUT.c.err" || fail "C compile failed"
$PLI "$OUT.pli.o" "$OUT.c.o" -o "$OUT.exe" 2>"$OUT.link.err" || fail "link failed"

out=$("$OUT.exe" 2>&1) || fail "the mirrored program failed: $out"
echo "$out" | grep -q '^PASS struct_mirror$' || fail "interop mismatch: $out"

# The standalone C program must compute the same checksum as the PL/I one.
$CLANG -O2 -DSM_STANDALONE "$OUT.c" -o "$OUT.vec" 2>"$OUT.c.err" || fail "standalone C build failed"
cout=$("$OUT.vec" 2>&1) || fail "the standalone C program failed: $cout"
pout=$(echo "$out" | grep '^checksum ' | head -1)
[ "$pout" = "$cout" ] || fail "checksum differs: PL/I '$pout' vs C '$cout'"

# --- 3. the vectorization: both builds must reach the same verdict ----------
# --keep-ll writes the optimized IR (it runs after the -O pipeline).
$PLI --keep-ll -O2 -c "$OUT.pli" -o "$OUT/struct_mirror.o" 2>"$OUT.pli.err" || fail "PL/I -O2 build failed"
$CLANG -O2 -DSM_STANDALONE -emit-llvm -S "$OUT.c" -o "$OUT.vec.ll" 2>"$OUT.c.err" || fail "C -O2 IR failed"

pli_fn=$(awk '/^define .*@PLI_STRUCT_MIRROR/,/^}/' "$OUT/struct_mirror.ll")
c_fn=$(awk '/^define .*@main/,/^}/' "$OUT.vec.ll")
[ -n "$pli_fn" ] || fail "no PL/I procedure in the optimized IR"
[ -n "$c_fn" ] || fail "no C main in the optimized IR"

# A vector type in the function body means the loop vectorized.
pli_vec=$(echo "$pli_fn" | grep -c '<[0-9][0-9]* x ')
c_vec=$(echo "$c_fn" | grep -c '<[0-9][0-9]* x ')
[ "$pli_vec" -gt 0 ] && pli_v=yes || pli_v=no
[ "$c_vec" -gt 0 ] && c_v=yes || c_v=no
[ "$pli_v" = "$c_v" ] || fail "vectorization differs: PL/I $pli_v vs C $c_v"

echo "PASS struct-mirror"
exit 0
