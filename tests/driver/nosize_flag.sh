#!/bin/sh
# tests/driver/nosize_flag.sh — --no-size-checks disables the SIZE
# traps program-wide: an overflowing computation with no (NOSIZE)
# prefix wraps instead of aborting.
set -u
PLIC=./build/plic
OUT=tests/driver/out/nosize_flag
mkdir -p tests/driver/out

cat > "$OUT.pli" <<'EOF'
nsflag: procedure options(main);
   declare x fixed bin(31);
   x = 2147483647 + 1;
   put skip list(x);
end nsflag;
EOF

$PLIC "$OUT.pli" --no-size-checks -o "$OUT" || { echo "FAIL nosize-flag: compile"; exit 1; }
out=$("$OUT" 2>&1); rc=$?
case "$out" in
  *-2147483648*) ;;
  *) echo "FAIL nosize-flag: wrong output: $out"; exit 1;;
esac
if [ "$rc" -ne 0 ]; then echo "FAIL nosize-flag: exited $rc"; exit 1; fi
echo "PASS nosize-flag"
exit 0
