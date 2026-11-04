#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/range_facts.sh — D1 (ADR-365): the declared-precision !range
# fact must never change an answer. The fact claims [-2^p, 2^p-1]; each case
# puts a value outside that claim on the storage it describes, so the wrong
# claim would fold a comparison to the wrong answer. The --no-range-facts
# pairs re-run the same program with the fact suppressed (the differential
# oracle): both paths must print the same result.
set -u
# Windows (MSYS/MinGW/Cygwin sh): linked binaries need a .exe suffix and
# program output uses CRLF. On Unix EXE is empty and stripping CR is a no-op.
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
  *) EXE= ;;
esac
PLI=${PLI:-./build/cmake/pli}
OUT=./tests/driver/out/range_facts
mkdir -p tests/driver/out

# The value source: without LTO the call is opaque, so the optimizer has only
# the range fact to decide what a load may hold (not a constant fold).
cat > "$OUT.lib.pli" <<'EOF'
 v: procedure(n) returns(fixed bin(31));
    declare n fixed bin(31);
    if n = 1 then return(32767);
    if n = 2 then return(20000);
    return(2147483647);
 end v;
EOF

# run SRC LABEL FLAGS WANT — compile $OUT.$SRC.pli with the shared library,
# run it, and compare stdout exactly.
run() {
  src=$1; label=$2; flags=$3; want=$4
  # shellcheck disable=SC2086
  $PLI "$OUT.$src.pli" "$OUT.lib.pli" $flags -o "$OUT.$label$EXE" || {
    echo "FAIL range-facts: $label compile"; exit 1; }
  got=$("$OUT.$label$EXE" | tr -d '\r')
  if [ "$got" = "$want" ]; then
    echo "PASS range-facts-$label"
  else
    echo "FAIL range-facts: $label output '$got', want '$want'"
    exit 1
  fi
}

# The top of FIXED BINARY(15)'s declared range: the old p-bit fact said
# [-16384, 16384), so 32767 > 16383 folded to false. ADR-282's range is
# [-32768, 32768), so the comparison is live and true.
cat > "$OUT.top_range.pli" <<'EOF'
 top: procedure options(main);
    declare v entry(fixed bin(31)) returns(fixed bin(31));
    declare a(4) fixed bin(15);
    declare i fixed bin(31);
    i = 1;
    a(i) = v(1);
    put skip list(a(i) > 16383);
 end top;
EOF
run top_range top_range "--size-checks -O2" "1"
run top_range top_range_default "-O2" "1"
run top_range top_range_oracle "--size-checks -O2 --no-range-facts" "1"

# FIXED BINARY(31) fills its slot, so it has no narrower fact at all; the
# value must survive both the checked and the default (checks-off) build.
cat > "$OUT.slot_full.pli" <<'EOF'
 slot: procedure options(main);
    declare v entry(fixed bin(31)) returns(fixed bin(31));
    declare a(4) fixed bin(31);
    declare i fixed bin(31);
    i = 1;
    a(i) = v(3);
    put skip list(a(i) > 1073741824);
 end slot;
EOF
run slot_full slot_full "-O2" "1"
run slot_full slot_full_checked "--size-checks -O2" "1"

# A (NOSIZE) store may leave the declared precision, so the storage carries
# no fact: 40000 is stored as-is and is greater than 32767.
cat > "$OUT.nosize_write.pli" <<'EOF'
 nsw: procedure options(main);
    declare a(4) fixed bin(15);
    declare i fixed bin(31);
    i = 1;
    (nosize): a(i) = 40000;
    put skip list(a(i) > 32767);
 end nsw;
EOF
run nosize_write nosize_write "--size-checks -O2" "1"
run nosize_write nosize_write_default "-O2" "1"
run nosize_write nosize_write_oracle "--size-checks -O2 --no-range-facts" "1"

# The callee writes the caller's storage through a reference under (NOSIZE):
# the actual's address escapes, so the caller's own load carries no fact.
cat > "$OUT.ref_write.pli" <<'EOF'
 rw: procedure options(main);
    declare a fixed bin(15);
    a = 1;
    call w(a);
    put skip list(a > 32767);
 end rw;
 w: procedure(x);
    declare x fixed bin(15);
    (nosize): x = 40000;
 end w;
EOF
run ref_write ref_write "--size-checks -O2" "1"
run ref_write ref_write_default "-O2" "1"
run ref_write ref_write_oracle "--size-checks -O2 --no-range-facts" "1"

# A parameter is the caller's storage, so it has no fact of its own: the
# 20000 the caller passes must still compare greater than 16383.
cat > "$OUT.param_read.pli" <<'EOF'
 pr: procedure options(main);
    declare v entry(fixed bin(31)) returns(fixed bin(31));
    declare a fixed bin(15);
    a = v(2);
    call r(a);
 end pr;
 r: procedure(x);
    declare x fixed bin(15);
    put skip list(x > 16383);
 end r;
EOF
run param_read param_read "--size-checks -O2" "1"
run param_read param_read_default "-O2" "1"

# A UNION sibling shares the storage through another declaration, so the
# FIXED BINARY(15) member reads whatever the FIXED BINARY(31) sibling wrote.
cat > "$OUT.union_member.pli" <<'EOF'
 um: procedure options(main);
    declare 1 u cell,
      2 a fixed bin(31),
      2 b fixed bin(15);
    u.a = 40000;
    put skip list(u.b > 32767);
 end um;
EOF
run union_member union_member "--size-checks -O2" "1"
run union_member union_member_default "-O2" "1"

# ADDR exposes the storage to a BASED write with no check for this type.
cat > "$OUT.addr_write.pli" <<'EOF'
 aw: procedure options(main);
    declare a fixed bin(15);
    declare p pointer;
    declare w fixed bin(15) based(p);
    a = 1;
    p = addr(a);
    (nosize): w = 40000;
    put skip list(a > 32767);
 end aw;
EOF
run addr_write addr_write "--size-checks -O2" "1"
run addr_write addr_write_default "-O2" "1"

exit 0
