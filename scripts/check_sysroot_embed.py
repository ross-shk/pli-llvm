#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
"""scripts/check_sysroot_embed.py — assert the embedded link sysroot is complete.

The Windows build is the only target that compiles sysroot objects at build
time (gs_stubs.o, chkstk.o, ...) then embeds them via gen_sysroot_embed.py.
If one of those build-generated files is dropped (e.g. the historical
"gs_stubs.o missing from PLI_SYSROOT_FILES" bug), user links fail with an
incomplete-sysroot error that only surfaces at the Test step or later — well
after the seam rotted. This step inspects the generated manifest
(embedded_sysroot.inc) right after the Build step and fails fast if any
expected entry is absent.

Runs on the Windows CI job only (the Unix sysroots are checked-in sidecars
with no build-time embedding, so they cannot regress this way).

Usage:
  check_sysroot_embed.py <manifest.inc> <expected.name> [<expected.name> ...]
"""
import re
import sys


def parse_entries(manifest):
    """Parse the manifest and return the set of embedded sysroot paths."""
    with open(manifest, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    return set(re.findall(r'\{"([^"]+)",', text))


def main(argv):
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    manifest = argv[1]
    expected = set(argv[2:])
    if not expected:
        print("error: expected entry set is empty", file=sys.stderr)
        return 2
    try:
        entries = parse_entries(manifest)
    except OSError as e:
        print(
            f"FAIL: cannot read embedded sysroot manifest {manifest}: {e}",
            file=sys.stderr,
        )
        print(
            "  (manifest is generated during the build by "
            "scripts/gen_sysroot_embed.py; check the Build step)",
            file=sys.stderr,
        )
        return 1
    missing = sorted(expected - entries)
    extra = sorted(entries - expected)
    if missing:
        print(
            "FAIL: embedded sysroot incomplete; missing entries:\n  "
            + "\n  ".join(missing),
            file=sys.stderr,
        )
        print(
            f"embedded entries ({len(entries)}):\n  "
            + "\n  ".join(sorted(entries)),
            file=sys.stderr,
        )
        return 1
    if extra:
        print(
            "note: embedded sysroot has %d entry(ies) not in the expected "
            "set: %s" % (len(extra), ", ".join(extra)),
            file=sys.stderr,
        )
    print(
        "OK: embedded link sysroot complete (%d entries, %d expected)"
        % (len(entries), len(expected))
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
