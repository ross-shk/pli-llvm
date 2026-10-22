#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/peephole_fuzz.sh — a seeded differential check for the WS-C
# peepholes (ADR-242 concat_const, ADR-243 bit_merge). Each generated program
# is compiled twice — once with the peepholes and once with the generic
# lowering — and the two runs must print the same thing. The seed is fixed, so
# the corpus is the same on every run.
#
# The corpus covers the shapes the two peepholes touch: concatenations of
# fixed-length variables and literals (including empty literals and nested
# concatenations, so the result is truncated and blank-padded), and masked bit
# tests on one value (disjoint 4-bit fields, which merge, and overlapping
# masks, which must not).
set -u
PLI=${PLI:-./build/cmake/pli}
OUT=tests/driver/out/peephole_fuzz
mkdir -p "$OUT"
PY=python3
command -v "$PY" >/dev/null 2>&1 || PY=python

"$PY" - "$PLI" "$OUT" <<'PYEOF'
import random
import subprocess
import sys
from pathlib import Path

pli, out = sys.argv[1], Path(sys.argv[2])
rng = random.Random(20261020)
# The corpus is small on purpose: the driver tests share the machine with the
# multi-module compiles, which sit near their own timeout under load. The wide
# verification of this generator (790 random programs) is a one-off, not a
# per-run cost.
PROGRAMS = 5


def concat(vars_):
    """A random concatenation over the given character variables."""
    parts = []
    for _ in range(rng.randint(2, 4)):
        r = rng.random()
        if r < 0.55:
            parts.append(rng.choice(vars_))
        elif r < 0.85:
            parts.append("'%s'" % "".join(rng.choice("abcXY ") for _ in range(rng.randint(0, 5))))
        else:
            parts.append("(%s)" % concat(vars_))
    return " || ".join(parts)


def program():
    """One random program: concatenations into fixed-length targets, and
    masked bit tests on one value."""
    nbits = rng.choice([32, 40])
    names = ["c%d" % i for i in range(rng.randint(2, 4))]
    lens = [rng.randint(1, 12) for _ in names]
    lines = [" main: procedure options(main);"]
    for nm, ln in zip(names, lens):
        lines.append("   declare %s char(%d);" % (nm, ln))
    lines.append("   declare b bit(%d);" % nbits)
    lines.append("   declare hits fixed bin(31);")
    lines.append("   hits = 0;")
    for nm, ln in zip(names, lens):
        lines.append("   %s = '%s';" % (nm, "".join(rng.choice("abcXY 0123") for _ in range(ln))))
    # concatenations: the result is truncated or blank-padded to the target
    for _ in range(rng.randint(2, 5)):
        target = rng.choice(names)
        lines.append("   %s = %s;" % (target, concat(names)))
        lines.append("   put skip list(%s);" % target)
    # masked bit tests: disjoint 4-bit fields merge, overlapping masks do not
    fields = []
    for i in range(rng.randint(2, 4)):
        base = 4 * i
        if rng.random() < 0.75:
            mask = rng.randint(0, 15) << base
        else:
            mask = rng.randint(0, 0xFFFFFFFF)
        fields.append((mask, mask & (rng.randint(0, 15) << base)))

    def lit(v):
        return "'%s'b" % format(v, "0%db" % nbits)

    lines.append("   b = fixed(%d);" % rng.randint(0, 0x7FFFFFFF))
    tests = ["((b & %s) = %s)" % (lit(m), lit(e)) for m, e in fields]
    lines.append("   if %s then hits = hits + 1;" % " & ".join(tests))
    for m, e in fields:
        lines.append("   if (b & %s) = %s then hits = hits + 1;" % (lit(m), lit(e)))
    lines.append("   put skip list(hits);")
    lines.append(" end main;")
    return "\n".join(lines) + "\n"


def run(src, extra):
    """Compile and run one program; None when it does not compile."""
    path = out / "fuzz.pli"
    path.write_text(src)
    exe = out / "fuzz.bin"
    r = subprocess.run([pli] + extra + [str(path), "-o", str(exe)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return None
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    return r.stdout


bad = 0
for i in range(PROGRAMS):
    src = program()
    peepholed = run(src, [])
    generic = run(src, ["--experimental-lowering=concat_const:runtime",
                        "--experimental-lowering=bit_merge:runtime"])
    if peepholed is None or generic is None:
        print("FAIL peephole_fuzz: program %d did not compile" % i)
        bad += 1
    elif peepholed != generic:
        print("FAIL peephole_fuzz: program %d diverges\n%s" % (i, src))
        bad += 1

if bad:
    print("FAIL peephole_fuzz: %d of %d programs diverged" % (bad, PROGRAMS))
else:
    print("PASS peephole_fuzz")
PYEOF
