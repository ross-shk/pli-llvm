#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
"""tools/corpus_rank.py

Compile every `tests/**/*.pli` program with `pli -fsyntax-only` and tally the
emitted diagnostics by their `[TR 25.084 rule (nn)]` citation, printing a
ranked rule→count table. Highlights the B10-relevant rules:

  - rule (41) GENERIC selection
  - the keyword-abbreviation rules (statement/attribute keywords, per TR
    25.084 §2.3.2 and §2.3.3)
  - the 48-character-set operator rules (§2.3.3): AND/OR/CAT/GT/LT/GE/LE/NE
    already served; PT (->), .,. (;), .. (:), // (%) are the missing four.

Usage: corpus_rank.py [pli_binary]

Exits 0 when a ranking was produced; exits non-zero only when the toolchain is
missing and the run is skipped gracefully.
"""

import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PLI = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "build" / "cmake" / "pli")

# B10-relevant rule groups (TR 25.084).
B10_RULES = {41}  # GENERIC

# 48-character-set operators (§2.3.3): each is its own rule when unsupported.
OPERATOR_RULES = {115, 116, 117, 118, 119, 120, 121, 122}

# Abbreviation-related statement/attribute keyword rules (§2.3.2, §2.3.3).
KEYWORD_RULES = {
    7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
    33, 34, 35, 36, 37, 38, 39, 40, 42, 44, 48, 56, 61, 68, 69, 70, 71, 72, 73,
    74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85,
}

RULE_RE = re.compile(r"\[TR 25\.084 rule\s+\((\d+)\)\]")


def compile_one(src):
    """Run `pli -fsyntax-only <src>`; return (combined output text, error)."""
    try:
        proc = subprocess.run(
            [PLI, str(src), "-fsyntax-only"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=60,
        )
    except FileNotFoundError:
        return None, "missing toolchain: %s" % PLI
    except subprocess.TimeoutExpired:
        return None, "timeout: %s" % src
    return proc.stdout.decode("utf-8", "replace"), None


def main():
    if not Path(PLI).exists():
        print("skipping corpus ranking: pli binary not found at %s" % PLI,
              file=sys.stderr)
        return 0

    srcs = sorted(ROOT.glob("tests/**/*.pli"))
    counts = Counter()
    skipped = 0
    cited_total = 0
    for src in srcs:
        out, err = compile_one(src)
        if err:
            print("skip %s: %s" % (src.relative_to(ROOT), err), file=sys.stderr)
            skipped += 1
            continue
        cited_total += len(RULE_RE.findall(out))
        for m in RULE_RE.finditer(out):
            counts[int(m.group(1))] += 1

    total_progs = len(srcs) - skipped
    print("corpus: %d PL/I programs, %d rule-cited diagnostics (%d skipped)\n" %
          (total_progs, cited_total, skipped))

    # B10-relevant section first.
    print("=== B10-relevant rules ===")
    rel = sorted(set(B10_RULES) | OPERATOR_RULES | KEYWORD_RULES)
    rows = [(n, counts.get(n, 0)) for n in rel]
    rows.sort(key=lambda r: (-r[1], r[0]))
    for n, c in rows:
        tag = "GENERIC" if n == 41 else ("48-char" if n in OPERATOR_RULES else "abbrev")
        print("  rule (%2d): %6d  %s" % (n, c, tag))

    print("\n=== Full ranked rule→count table ===")
    for n, c in counts.most_common():
        tag = ""
        if n == 41:
            tag = "  <- GENERIC (B10)"
        elif n in OPERATOR_RULES:
            tag = "  <- 48-char operator (B10)"
        elif n in KEYWORD_RULES:
            tag = "  <- keyword abbreviation (B10)"
        print("  rule (%2d): %6d%s" % (n, c, tag))

    print("\ndone.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
