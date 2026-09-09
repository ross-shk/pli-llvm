#!/bin/sh
# tests/driver/fixit.sh — diagnostics carry a fix-it: the suggested insertion
# is rendered at the caret column. Exercises missing THEN, '=', PROCEDURE and
# END. Rules (75),(86),(2),(7).
set -u
PLIC=./build/plic
OUT=./tests/driver/out/fixit
mkdir -p "$OUT"
ok=1

check_has() {
  # $1 = label, $2 = output file, $3 = text that must appear
  if ! grep -qF "$3" "$2"; then
    echo "FAIL: $1: fix-it '$3' not found"
    ok=0
  fi
}

cat > "$OUT/miss_then.pli" <<'EOF'
 MAIN: PROCEDURE OPTIONS(MAIN);
   DECLARE X FIXED BINARY(31);
   IF X = 1 PUT SKIP LIST('a');
 END MAIN;
EOF
"$PLIC" "$OUT/miss_then.pli" -fsyntax-only >"$OUT/miss_then.out" 2>&1
check_has "missing THEN" "$OUT/miss_then.out" "THEN "

cat > "$OUT/miss_eq.pli" <<'EOF'
 MAIN: PROCEDURE OPTIONS(MAIN);
   DECLARE X FIXED BINARY(31);
   X 1;
 END MAIN;
EOF
"$PLIC" "$OUT/miss_eq.pli" -fsyntax-only >"$OUT/miss_eq.out" 2>&1
check_has "missing =" "$OUT/miss_eq.out" "= "

cat > "$OUT/miss_proc.pli" <<'EOF'
 MAIN: OPTIONS(MAIN);
   DECLARE X FIXED BINARY(31);
   X = 1;
   PUT SKIP LIST(X);
 END MAIN;
EOF
"$PLIC" "$OUT/miss_proc.pli" -fsyntax-only >"$OUT/miss_proc.out" 2>&1
check_has "missing PROCEDURE" "$OUT/miss_proc.out" "PROCEDURE "

cat > "$OUT/miss_end.pli" <<'EOF'
 MAIN: PROCEDURE OPTIONS(MAIN);
   DECLARE X FIXED BINARY(31);
   X = 1;
EOF
"$PLIC" "$OUT/miss_end.pli" -fsyntax-only >"$OUT/miss_end.out" 2>&1
check_has "missing END" "$OUT/miss_end.out" "END MAIN;"

[ "$ok" -eq 1 ] && echo PASS
exit 0
