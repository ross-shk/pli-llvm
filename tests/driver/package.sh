#!/bin/sh
# tests/driver/package.sh — PACKAGE blocks (extension, ADR-109): a
# library package exports PUB while PRIV stays module-private; the
# MAIN module calls PUB across the link. Each unit compiles with
# -c; cc links the objects with libpli. Symbol visibility is
# asserted with nm (T = global text, t = local).
set -u
PLIC=./build/plic
CC=clang
RTLIB=./build/libpli.a
OUT=tests/driver/out/package
mkdir -p tests/driver/out

cat > "$OUT.lib.pli" <<'EOF'
 lib: package exports(pub);
    pub: procedure(x) returns(fixed bin(31));
       declare x fixed bin(31);
       return(priv(x) * 2);
    end pub;
    priv: procedure(x) returns(fixed bin(31));
       declare x fixed bin(31);
       return(x + 1);
    end priv;
 end lib;
EOF
cat > "$OUT.main.pli" <<'EOF'
 packmain: procedure options(main);
    declare pub entry(fixed bin(31))
       returns(fixed bin(31)) external;
    declare r fixed bin(31);
    r = pub(10);
    if r ^= 22 then do;
       put skip list('FAIL r =', r);
    end;
    else put skip list('PASS package');
 end packmain;
EOF

$PLIC "$OUT.lib.pli" -c -o "$OUT.lib.o" || { echo "FAIL package: lib compile"; exit 1; }
$PLIC "$OUT.main.pli" -c -o "$OUT.main.o" || { echo "FAIL package: main compile"; exit 1; }
nm "$OUT.lib.o" | grep ' T _PUB' >/dev/null || { echo "FAIL package: _PUB not exported"; exit 1; }
if nm "$OUT.lib.o" | grep ' T _PRIV' >/dev/null; then echo "FAIL package: _PRIV leaked"; exit 1; fi
$CC "$OUT.main.o" "$OUT.lib.o" "$RTLIB" -o "$OUT" || { echo "FAIL package: link"; exit 1; }
"$OUT"
exit 0
