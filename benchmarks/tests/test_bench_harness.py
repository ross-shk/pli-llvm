#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
"""Self-tests for the benchmark harnesses (run: python3 -m unittest discover).

The harness's job is to turn program runs into numbers. These tests pin the
rule that makes the numbers trustworthy: a run that crashes, times out or
prints the wrong thing is never admitted as a measurement, and a gate is
never reported over a partial or mismatched corpus.
"""
import sys
import unittest
from pathlib import Path

BENCH = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(BENCH))

import bench_common  # noqa: E402
import run_benchmarks as rb  # noqa: E402
import run_tasking_bench as tb  # noqa: E402

PY = sys.executable


def _fake_results(ratio=0.5, match="✓"):
    """A results map in which every gate row is valid."""
    return {name: {"-O2": {"status": "ok", "match": match, "ratio": ratio}}
            for name in rb.GATE_BENCHMARKS}


class RunMeasuredTests(unittest.TestCase):
    """A timing sample comes only from a successful, expected run."""

    def test_success_returns_time_and_stdout(self):
        secs, out = bench_common.run_measured(
            [PY, "-c", "print('done')"], expected_stdout="done")
        self.assertGreaterEqual(secs, 0.0)
        self.assertEqual(out.strip(), "done")

    def test_nonzero_exit_is_rejected(self):
        with self.assertRaises(bench_common.MeasureError) as ctx:
            bench_common.run_measured([PY, "-c", "import sys; sys.exit(3)"])
        self.assertEqual(ctx.exception.status, "run_failed")

    def test_wrong_output_is_rejected(self):
        with self.assertRaises(bench_common.MeasureError) as ctx:
            bench_common.run_measured([PY, "-c", "print('nope')"],
                                      expected_stdout="done")
        self.assertEqual(ctx.exception.status, "output_mismatch")

    def test_timeout_is_rejected(self):
        with self.assertRaises(bench_common.MeasureError) as ctx:
            bench_common.run_measured([PY, "-c", "import time; time.sleep(5)"],
                                      timeout=0.2)
        self.assertEqual(ctx.exception.status, "run_timeout")

    def test_missing_binary_is_rejected(self):
        with self.assertRaises(bench_common.MeasureError) as ctx:
            bench_common.run_measured(["definitely-not-a-binary-xyz"])
        self.assertEqual(ctx.exception.status, "spawn_failed")

    def test_pinning_probe_reports_an_outcome(self):
        info = bench_common.probe_pinning()
        for key in ("mode", "prefix", "applied", "note"):
            self.assertIn(key, info)
        # An unapplied prefix must be empty: the runner applies what it records.
        if not info["applied"]:
            self.assertEqual(info["prefix"], [])


class ParityTests(unittest.TestCase):
    """Which rows may feed a ratio, and which may feed the gate."""

    def test_invalid_rows(self):
        ok = {"status": "ok", "match": "✓", "ratio": 1.0}
        self.assertTrue(rb.row_is_valid(ok))
        for row in ({"status": "run_failed"},
                    {"status": "ok", "match": "✗", "ratio": 1.0},
                    {"status": "ok", "match": "✓", "ratio": float("nan")},
                    {"status": "ok", "match": "✓", "ratio": 0}):
            self.assertFalse(rb.row_is_valid(row), row)

    def test_descriptive_rows_never_gate(self):
        self.assertEqual(rb.outputs_agree("decimal_ops", "1", "2"), "~")
        self.assertEqual(rb.outputs_agree("collatz", "1", "1"), "✓")
        self.assertEqual(rb.outputs_agree("collatz", "1", "2"), "✗")
        self.assertIn("decimal_ops", rb.NON_PARITY)
        self.assertNotIn("decimal_ops", rb.GATE_BENCHMARKS)

    def test_geomean_requires_the_whole_corpus(self):
        self.assertAlmostEqual(rb.accepted_geomean(_fake_results(0.5),
                                                   rb.GATE_BENCHMARKS), 0.5)
        missing = _fake_results()
        del missing[rb.GATE_BENCHMARKS[0]]
        self.assertIsNone(rb.accepted_geomean(missing, rb.GATE_BENCHMARKS))

        failed = _fake_results()
        failed[rb.GATE_BENCHMARKS[0]]["-O2"]["status"] = "run_failed"
        self.assertIsNone(rb.accepted_geomean(failed, rb.GATE_BENCHMARKS))

        mismatched = _fake_results()
        mismatched[rb.GATE_BENCHMARKS[0]]["-O2"]["match"] = "✗"
        self.assertIsNone(rb.accepted_geomean(mismatched, rb.GATE_BENCHMARKS))

    def test_inventory_is_consistent(self):
        self.assertEqual(rb.validate_inventory(), [])

    def test_inventory_detects_a_missing_pair_source(self):
        bad = ("checks_on", "a", [], "b", [], ["lang/no_such_source.pli"])
        rb.PAIRS.append(bad)
        try:
            problems = rb.validate_inventory()
        finally:
            rb.PAIRS.pop()
        self.assertTrue(any("no_such_source" in p for p in problems), problems)


class TaskingRatioTests(unittest.TestCase):
    """A side that failed yields no ratio, whatever its wall time was."""

    def test_failed_side_has_no_ratio(self):
        ok = {"status": "ok", "median": 1.0, "min": 1.0}
        slower = {"status": "ok", "median": 2.0, "min": 2.0}
        failed = {"status": "run_failed"}
        self.assertEqual(tb._ratio(slower, ok), (2.0, 2.0))
        self.assertEqual(tb._ratio(ok, failed), (None, None))
        self.assertEqual(tb._ratio(failed, ok), (None, None))
        self.assertEqual(tb._ratio(ok, {"status": "skipped"}), (None, None))


class RegressionTests(unittest.TestCase):
    """A regression check says how many rows it actually compared."""

    def test_only_valid_rows_are_compared(self):
        current = {"a": {"-O2": {"status": "ok", "match": "✓", "ratio": 1.0}},
                   "b": {"-O2": {"status": "run_failed"}}}
        previous = {"a": {"-O2": {"status": "ok", "match": "✓", "ratio": 0.5}},
                    "b": {"-O2": {"status": "ok", "match": "✓", "ratio": 0.5}}}
        warnings, compared = rb.check_for_deterioration(
            current, previous, ["a", "b"])
        self.assertEqual(compared, 1)
        self.assertEqual(len(warnings), 1)

    def test_manifests_must_match(self):
        manifest = {"benchmarks": ["a"], "opt_levels": ["-O2"], "runs": 5,
                    "warmup": 1, "checks_forced": False,
                    "extra_pli_flags": [""], "platform": "x"}
        self.assertIsNone(rb.manifest_mismatch(manifest, {"manifest": manifest}))
        other = dict(manifest, runs=3)
        self.assertIn("runs", rb.manifest_mismatch(manifest, {"manifest": other}))
        self.assertIsNotNone(rb.manifest_mismatch(manifest, {}))


if __name__ == "__main__":
    unittest.main()
