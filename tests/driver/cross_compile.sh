#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/cross_compile.sh — the cross-target model (ADR-234).
#
# Two things are checked, and they hold in every build:
#
#   1. A target with no runtime bitcode is *diagnosed*, never silently
#      served with the host's runtime (that would put a host-ABI runtime into
#      a foreign object).
#   2. A target with a runtime bitcode compiles to an object of that target's
#      format, and the ABI families stay apart: a Windows MSVC request must
#      not pick the MinGW runtime, and a musl spelling of a Linux target must
#      pick the gnu one (same libc, same ABI).
#
# The artifact checks run only when the build embeds the target's runtime
# (configure with -DPLI_CROSS_BITCODE=ON and prepare the sysroots); the
# diagnostic checks always run.
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=tests/driver/out/cross_compile
mkdir -p "$OUT"

ok=1
fail() { echo "FAIL: $1"; ok=0; }

cat > "$OUT.witness.pli" <<'EOF'
witness: procedure options(main);
   put skip list('PASS witness');
end witness;
EOF

# The targets this build embeds a runtime for.
EMBEDDED=$($PLI version 2>/dev/null | sed -n 's/^cross targets: //p')
host=$($PLI version 2>/dev/null | sed -n 's/.*(LLVM [^,]*, \([^,]*\),.*/\1/p')

has_runtime() {
  for t in $EMBEDDED; do
    [ "$t" = "$1" ] && return 0
  done
  return 1
}

# 1. A target with no runtime is diagnosed, not silently served.
for triple in x86_64-unknown-linux-gnu aarch64-unknown-linux-gnu x86_64-pc-windows-gnu \
              aarch64-pc-windows-gnu x86_64-pc-windows-msvc; do
  if has_runtime "$triple"; then
    continue
  fi
  if $PLI --triple "$triple" -c "$OUT.witness.pli" -o "$OUT/x.o" >"$OUT.err" 2>&1; then
    fail "$triple compiled without a runtime"
  elif ! grep -q "no embedded runtime for target" "$OUT.err"; then
    fail "$triple: the missing-runtime diagnostic is missing"
    cat "$OUT.err"
  fi
done

# 2. A target with a runtime compiles to an object of that target's format.
check_object() {
  triple=$1
  pattern=$2
  if ! $PLI --triple "$triple" -c "$OUT.witness.pli" -o "$OUT/x.o" >"$OUT.err" 2>&1; then
    fail "$triple: compile failed"
    cat "$OUT.err"
    return
  fi
  if ! command -v file >/dev/null 2>&1; then
    return
  fi
  file "$OUT/x.o" | grep -qi "$pattern" ||
    fail "$triple: wrong object format ($(file -b "$OUT/x.o"))"
}

has_runtime x86_64-unknown-linux-gnu && check_object x86_64-unknown-linux-gnu "ELF 64-bit.*x86-64"
has_runtime aarch64-unknown-linux-gnu && check_object aarch64-unknown-linux-gnu "ELF 64-bit.*ARM aarch64"
has_runtime x86_64-pc-windows-gnu && check_object x86_64-pc-windows-gnu "PE32.*x86-64"
has_runtime aarch64-apple-macosx15.0 && check_object aarch64-apple-macosx15.0 "Mach-O 64-bit.*arm64"

# 3. The ABI families stay apart: a musl spelling of a Linux target uses the
#    same runtime (same libc), an MSVC request does not use the MinGW one.
if has_runtime x86_64-unknown-linux-gnu; then
  if ! $PLI --triple x86_64-unknown-linux-musl -c "$OUT.witness.pli" -o "$OUT/x.o" \
       >"$OUT.err" 2>&1; then
    fail "the musl spelling of a Linux target was not served"
    cat "$OUT.err"
  fi
fi
if has_runtime x86_64-pc-windows-gnu; then
  if $PLI --triple x86_64-pc-windows-msvc -c "$OUT.witness.pli" -o "$OUT/x.o" \
       >"$OUT.err" 2>&1; then
    fail "the MSVC request picked the MinGW runtime"
  fi
fi

# 4. The host still works (the family key must not break the host link).
if ! $PLI "$OUT.witness.pli" -o "$OUT/witness.bin" >"$OUT.err" 2>&1; then
  fail "the host link broke"
  cat "$OUT.err"
elif [ "$("$OUT/witness.bin" 2>/dev/null | tr -d '\r')" != "PASS witness" ]; then
  fail "the host program did not run"
fi

if [ "$ok" -eq 1 ]; then
  echo PASS
  exit 0
fi
exit 1
