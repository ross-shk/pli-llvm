#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/lifetime_asan.sh — the ADR-246 lifetime markers under
# AddressSanitizer. The markers tell LLVM that a stack buffer dies at the
# end of the call, so that several calls can share one slot; a marker in
# the wrong place would let a buffer be reused while it is still live. The
# program exercises the three marked buffers (the argument dummy, the
# CHAR(*) VARYING copy and the VARYING copy-back) and checks its own
# values, so a wrongly ended lifetime shows up as a wrong answer as well as
# a sanitizer report.
#
# The sanitizer runtime has to match the object the compiler produced, so
# the clang comes from the same LLVM installation as the compiler (found
# through the build tree's CMake cache). Without such a clang the test
# skips rather than fails.
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=tests/driver/out/lifetime_asan
mkdir -p tests/driver/out

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
     echo "SKIP: the sanitizer runtime setup is Unix-only"
     exit 0
     ;;
esac

# The clang of the compiler's own LLVM: its sanitizer runtime matches the
# object, another clang's does not.
CC=""
if [ -n "${LLVM_BINDIR:-}" ] && [ -x "$LLVM_BINDIR/clang" ]; then
  CC="$LLVM_BINDIR/clang"
elif [ -f build/cmake/CMakeCache.txt ]; then
  llvm_dir=$(sed -n 's/^LLVM_DIR:[A-Z_]*=//p' build/cmake/CMakeCache.txt | head -1)
  if [ -n "$llvm_dir" ]; then
    # <prefix>/lib/cmake/llvm -> <prefix>
    prefix=$(dirname "$(dirname "$(dirname "$llvm_dir")")")
    if [ -x "$prefix/bin/clang" ]; then
      CC="$prefix/bin/clang"
    fi
  fi
fi
if [ -z "$CC" ]; then
  echo "SKIP: no clang from the compiler's own LLVM (set LLVM_BINDIR)"
  exit 0
fi

cat > "$OUT.pli" <<'EOF'
 /* ADR-246: the argument dummy, the CHAR(*) VARYING copy and the VARYING
    copy-back are the buffers whose lifetime is marked. */
 lifetime_asan: procedure options(main);
    declare c char(10);
    declare v char(20) varying;
    declare i fixed bin(31);
    declare fails fixed bin(31);
    fails = 0;

    /* a character expression passed to a procedure: the argument dummy */
    do i = 1 to 50;
       call takes_char('abc');
    end;

    /* a non-VARYING actual into a CHAR(*) VARYING parameter: the vstar copy */
    if show_len(c) ^= 10 then fails = fails + 1;

    /* a VARYING argument copied back into the caller after the call */
    v = 'hello';
    call grow(v);
    if length(v) ^= 10 then fails = fails + 1;
    if substr(v, 1, 5) ^= 'hello' then fails = fails + 1;

    if fails = 0 then put skip list('PASS lifetime_asan');
    else put skip list('FAIL lifetime_asan: fails=', fails);

    takes_char: procedure(s);
       declare s char(10);
       s = s || 'x';
    end takes_char;

    show_len: procedure(s) returns(fixed bin(31));
       declare s char(*) varying;
       return(length(s));
    end show_len;

    grow: procedure(buf);
       declare buf char(4096) varying;
       buf = buf || 'hello';
    end grow;
 end lifetime_asan;
EOF

$PLI "$OUT.pli" -c -o "$OUT.o" || { echo "FAIL lifetime_asan: compile"; exit 1; }
"$CC" -fsanitize=address "$OUT.o" -o "$OUT" 2>"$OUT.link.log" || {
   if grep -q "sanitiz" "$OUT.link.log"; then
      echo "SKIP: no AddressSanitizer runtime in this toolchain"
      exit 0
   fi
   echo "FAIL lifetime_asan: link"
   cat "$OUT.link.log"
   exit 1
}

out=$("$OUT" 2>&1)
case "$out" in
  *AddressSanitizer*)
     echo "FAIL lifetime_asan: sanitizer report"
     echo "$out" | head -20
     exit 1
     ;;
esac
case "$out" in
  *"PASS lifetime_asan"*) echo "PASS lifetime_asan" ;;
  *) echo "FAIL lifetime_asan: $out"; exit 1 ;;
esac
exit 0
