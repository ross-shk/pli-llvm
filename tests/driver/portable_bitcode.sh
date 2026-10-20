#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/portable_bitcode.sh — the portable output mode (ADR-236).
#
# A target this compiler knows nothing about must still produce something
# the user can finish with their own toolchain. The contract of the portable
# artifact:
#   - it is written (the requested executable becomes <output>.bc);
#   - it parses as LLVM bitcode;
#   - it carries NO data layout (the consumer's toolchain applies the
#     target's own);
#   - the pli_* runtime symbols stay UNDEFINED (the user links the runtime
#     they built for their target).
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=tests/driver/out/portable_bitcode
mkdir -p "$OUT"

# The LLVM tools (llvm-dis, llvm-nm) sit next to the clang the tests use.
TOOLS=""
if [ -n "${CLANG:-}" ] && [ -x "${CLANG}" ]; then
  TOOLS=$(dirname "$CLANG")
elif command -v llvm-dis >/dev/null 2>&1; then
  TOOLS=$(dirname "$(command -v llvm-dis)")
fi

ok=1
fail() { echo "FAIL: $1"; ok=0; }

cat > "$OUT.witness.pli" <<'EOF'
witness: procedure options(main);
   put skip list('PASS witness');
end witness;
EOF

# --- 1. an unknown architecture: portable bitcode, not an error -------------
# The triple is deliberately one no LLVM backend knows.
UNKNOWN=e2k-unknown-linux-gnu
rm -f "$OUT/prog" "$OUT/prog.bc"
if "$PLI" --triple "$UNKNOWN" "$OUT.witness.pli" -o "$OUT/prog" >"$OUT.err" 2>&1; then
  : # the fallback succeeds (an artifact was produced)
else
  fail "the portable fallback exited non-zero"
  cat "$OUT.err"
fi
if [ ! -f "$OUT/prog.bc" ]; then
  fail "no portable bitcode was written"
  cat "$OUT.err"
fi
if ! grep -q "portable bitcode" "$OUT.err"; then
  fail "the portable diagnostic is missing"
  cat "$OUT.err"
fi
if ! grep -q "rt_\*\.c" "$OUT.err"; then
  fail "the runtime recipe is missing from the diagnostic"
  cat "$OUT.err"
fi

# --- 2. the artifact's contract --------------------------------------------
if [ -n "$TOOLS" ] && [ -x "$TOOLS/llvm-dis" ]; then
  if "$TOOLS/llvm-dis" "$OUT/prog.bc" -o "$OUT/prog.ll" 2>"$OUT.err"; then
    if grep -q "target datalayout" "$OUT/prog.ll"; then
      fail "the portable bitcode carries a data layout"
    fi
    if ! grep -q "target triple = \"$UNKNOWN\"" "$OUT/prog.ll"; then
      fail "the portable bitcode lost the target triple"
    fi
  else
    fail "the portable bitcode does not parse"
    cat "$OUT.err"
  fi
  if [ -x "$TOOLS/llvm-nm" ]; then
    undef=$("$TOOLS/llvm-nm" --undefined-only "$OUT/prog.bc" 2>/dev/null | grep -c "pli_")
    def=$("$TOOLS/llvm-nm" --defined-only "$OUT/prog.bc" 2>/dev/null | grep -c "pli_")
    [ "$undef" -gt 0 ] || fail "the runtime symbols are not undefined (the runtime was merged)"
    [ "$def" -eq 0 ] || fail "the portable bitcode defines runtime symbols"
  fi
fi

# --- 3. the textual form carries no layout either -------------------------
rm -f "$OUT/prog.ll"
if "$PLI" --emit-llvm --triple "$UNKNOWN" "$OUT.witness.pli" -o "$OUT/prog.ll" >/dev/null 2>&1; then
  if grep -q "target datalayout" "$OUT/prog.ll"; then
    fail "the portable IR text carries a data layout"
  fi
else
  fail "the portable IR text was not written"
fi

# --- 4. --no-portable turns the fallback into an error ---------------------
if "$PLI" --no-portable --triple "$UNKNOWN" "$OUT.witness.pli" -o "$OUT/prog2" \
     >"$OUT.err" 2>&1; then
  fail "--no-portable did not fail"
else
  grep -q "no LLVM backend" "$OUT.err" || fail "the --no-portable diagnostic is missing"
fi

# --- 5. --emit-bitcode works for a target this build knows ----------------
rm -f "$OUT/host.bc"
if "$PLI" --emit-bitcode "$OUT.witness.pli" -o "$OUT/host.bc" >"$OUT.err" 2>&1; then
  if [ ! -f "$OUT/host.bc" ]; then
    fail "--emit-bitcode wrote no file"
  fi
  if grep -q "no LLVM backend" "$OUT.err"; then
    fail "the host was reported as having no backend"
    cat "$OUT.err"
  fi
else
  fail "--emit-bitcode failed"
  cat "$OUT.err"
fi

# --- 6. the host is unaffected --------------------------------------------
if ! "$PLI" "$OUT.witness.pli" -o "$OUT/witness.bin" >"$OUT.err" 2>&1; then
  fail "the host build broke"
  cat "$OUT.err"
elif [ "$("$OUT/witness.bin" 2>/dev/null | tr -d '\r')" != "PASS witness" ]; then
  fail "the host program did not run"
fi

if [ "$ok" -eq 1 ]; then
  echo PASS
  exit 0
fi
exit 1
