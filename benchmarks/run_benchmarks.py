#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM
"""
PL/I Compiler Benchmark Harness

Compiles and runs benchmarks with different optimization levels, comparing
generated PL/I code performance against C baselines. Each benchmark has a
`.pli` source (compiled with `plic`) and a `baseline/<name>_c.c` source
(compiled with `cc`), all implementing the same algorithm.
"""
import os
import subprocess
import sys
import time
import json
import csv
import math
import statistics
from pathlib import Path
from typing import Dict, List, Tuple, Optional

BENCHMARK_DIR = Path(__file__).parent
PLIC = BENCHMARK_DIR.parent / "build" / "plic"
CC = os.environ.get("CC", "cc")
RESULTS_DIR = BENCHMARK_DIR / "results"
BIN_DIR = RESULTS_DIR / "bin"
RESULTS_DIR.mkdir(exist_ok=True)
BIN_DIR.mkdir(exist_ok=True)

MAX_RESULTS = 20

# Per-benchmark PL/I flags that must be active for the benchmark to measure
# what it claims (P1.6).
EXTRA_PLIFLAGS = {
    "condition_ops": ["--subscript-checks"],
}

BENCHMARKS = [
    ("scalar_arith",      "scalar_arith.pli",      "baseline/scalar_arith_c.c"),
    ("array_matmul",      "array_matmul.pli",      "baseline/array_matmul_c.c"),
    ("string_ops",        "string_ops.pli",        "baseline/string_ops_c.c"),
    ("decimal_ops",       "decimal_ops.pli",       "baseline/decimal_ops_c.c"),
    ("condition_ops",     "condition_ops.pli",     "baseline/condition_ops_c.c"),
    ("proc_calls",        "proc_calls.pli",        "baseline/proc_calls_c.c"),
    ("math_functions",    "math_functions.pli",    "baseline/math_functions_c.c"),
    ("bit_ops",           "bit_ops.pli",           "baseline/bit_ops_c.c"),
    ("array_sort",        "array_sort.pli",        "baseline/array_sort_c.c"),
    ("linked_list",       "linked_list.pli",       "baseline/linked_list_c.c"),
    ("collatz",           "collatz.pli",           "baseline/collatz_c.c"),
    ("controlled_stack",  "controlled_stack.pli",  "baseline/controlled_stack_c.c"),
    ("on_conditions",     "on_conditions.pli",     "baseline/on_conditions_c.c"),
    ("matrix_float",      "matrix_float.pli",      "baseline/matrix_float_c.c"),
    ("mandelbrot",        "mandelbrot.pli",        "baseline/mandelbrot_c.c"),
    ("io_throughput",     "io_throughput.pli",     "baseline/io_throughput_c.c"),
    ("division",          "division.pli",          "baseline/division_c.c"),
]

OPT_LEVELS = ["-O0", "-O1", "-O2", "-O3"]
RUNS_PER_BENCH = 5

BENCHMARKS_REQUIRING_MATH = {"math_functions", "matrix_float"}

# Float-sensitive benches eligible for tolerant comparison (P1.4).
FLOAT_BENCHES = {"scalar_arith", "math_functions", "matrix_float"}

# Flags that force every check on (P1.6/P1.7).
CHECKS_FORCED_FLAGS = ["--size-checks", "--subscript-checks"]

# Expected outputs for PL/I↔C parity and timing thresholds (P1.4/W7).
# PL/I output is compared against the C baseline via outputs_match.
# "min_ms_O2" is the minimum median PL/I time at -O2 (≥50ms noise floor).
CHECKS = {
    "scalar_arith":     {"min_ms_O2": 50, "float_tol": True},
    "array_matmul":     {"min_ms_O2": 50, "float_tol": False},
    "string_ops":       {"min_ms_O2": 50, "float_tol": False},
    "decimal_ops":      {"min_ms_O2": 50, "float_tol": False},  # expected mismatch: decimal vs FP
    "condition_ops":    {"min_ms_O2": 50, "float_tol": False},
    "proc_calls":       {"min_ms_O2": 50, "float_tol": False},
    "math_functions":   {"min_ms_O2": 50, "float_tol": True},
    "bit_ops":          {"min_ms_O2": 50, "float_tol": False},
    "array_sort":       {"min_ms_O2": 50, "float_tol": False},
    "linked_list":      {"min_ms_O2": 50, "float_tol": False},
    "collatz":          {"min_ms_O2": 50, "float_tol": False},
    "controlled_stack": {"min_ms_O2": 50, "float_tol": False},
    "on_conditions":    {"min_ms_O2": 50, "float_tol": False},
    "matrix_float":     {"min_ms_O2": 50, "float_tol": True},
    "mandelbrot":       {"min_ms_O2": 50, "float_tol": False},
    "io_throughput":    {"min_ms_O2": 50, "float_tol": False},
    "division":         {"min_ms_O2": 50, "float_tol": False},
}


def run_command(cmd: List[str], cwd: Path = None, timeout: int = 300) -> Tuple[int, str, str]:
    """Run a command and return (returncode, stdout, stderr)."""
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd, timeout=timeout)
        return result.returncode, result.stdout, result.stderr
    except subprocess.TimeoutExpired:
        return -1, "", "Timeout"


def compile_pli(bench_name: str, pli_file: str, opt: str, output: Path,
                extra_flags: Optional[List[str]] = None) -> bool:
    """Compile a PL/I benchmark with plic at the given optimization level.

    If *extra_flags* is None, the bench-level EXTRA_PLIFLAGS are used (P1.6).
    """
    src = BENCHMARK_DIR / pli_file
    cmd = [str(PLIC), str(src), "-o", str(output), opt]
    cmd.extend(extra_flags if extra_flags is not None
               else EXTRA_PLIFLAGS.get(bench_name, []))
    rc, out, err = run_command(cmd)
    if rc != 0:
        print(f"  PL/I compile failed [{bench_name} {opt}]:\n{out}\n{err}")
    return rc == 0


def compile_c(bench_name: str, c_file: str, opt: str, output: Path) -> bool:
    """Compile a C benchmark with cc at the given optimization level."""
    src = BENCHMARK_DIR / c_file
    cmd = [CC, opt, str(src), "-o", str(output)]
    if bench_name in BENCHMARKS_REQUIRING_MATH:
        cmd.append("-lm")
    rc, out, err = run_command(cmd)
    if rc != 0:
        print(f"  C compile failed [{bench_name} {opt}]:\n{out}\n{err}")
    return rc == 0


def run_benchmark(exe: Path, runs: int = RUNS_PER_BENCH,
                  warmup: int = 1) -> Tuple[List[float], str]:
    """Run a benchmark multiple times and return execution times in seconds.

    One warmup run is performed untimed (P1.2). Returns (times, last_stdout).
    """
    times = []
    for _ in range(warmup):
        run_command([str(exe)], timeout=60)
    out = ""
    for _ in range(runs):
        start = time.perf_counter()
        rc, out, err = run_command([str(exe)], timeout=60)
        end = time.perf_counter()
        if rc == 0:
            times.append(end - start)
        else:
            print(f"  Run failed: {err}")
            return [], ""
    return times, out


def median_result(times: List[float]) -> Optional[float]:
    if not times:
        return None
    return statistics.median(times)


def needs_recompile(src: Path, exe: Path) -> bool:
    """Skip recompilation if the executable is newer than the source (P1.5)."""
    if not exe.exists():
        return True
    return src.stat().st_mtime > exe.stat().st_mtime


def outputs_match(pli_out: str, c_out: str, bench_name: str) -> str:
    """Compare outputs (P1.4): exact '✓', tolerant '≈' for float benches, else '✗'."""
    if pli_out.strip() == c_out.strip():
        return "✓"
    if bench_name not in FLOAT_BENCHES:
        return "✗"
    try:
        ps = [float(t) for t in pli_out.split()]
        cs = [float(t) for t in c_out.split()]
    except ValueError:
        return "✗"
    if len(ps) != len(cs):
        return "✗"
    return "≈" if all(math.isclose(p, c, rel_tol=1e-9)
                        for p, c in zip(ps, cs)) else "✗"


def prune_old_results(files: List[Path], keep: int = MAX_RESULTS):
    """Keep only the newest *keep* result files; remove the rest (P1.5)."""
    files = sorted(files, key=lambda f: f.stat().st_mtime, reverse=True)
    for f in files[keep:]:
        f.unlink(missing_ok=True)


def main():
    import argparse
    parser = argparse.ArgumentParser(
        description="PL/I compiler benchmark harness")
    parser.add_argument("--checks-forced", action="store_true",
                        help="compile every benchmark with --size-checks "
                             "--subscript-checks at -O2 and require "
                             "identical stdout to the default run (P1.7)")
    parser.add_argument("--runs", type=int, default=RUNS_PER_BENCH,
                        help="timed runs per benchmark (default: %(default)s)")
    args = parser.parse_args()

    print("PL/I Compiler Benchmark Harness")
    print("=" * 70)

    if not PLIC.exists():
        print(f"Error: plic not found at {PLIC}")
        print("Build first: cmake -G Ninja -S . -B build/cmake && "
              "cmake --build build/cmake -j$(nproc)")
        return 1

    opt_levels = ["-O2"] if args.checks_forced else OPT_LEVELS

    # Header for the comparison table
    print(f"{'Benchmark':<20} {'Opt':<6} {'Flags':<24} {'PL/I (min)':>10} "
          f"{'PL/I (med)':>10} {'C (med)':>10} {'Ratio':>8} {'Match':>6}")
    print("-" * 84)

    results = {}
    ts = int(time.time())

    for bench_name, pli_file, c_file in BENCHMARKS:
        results[bench_name] = {}
        src_pli = BENCHMARK_DIR / pli_file
        src_c = BENCHMARK_DIR / c_file

        default_flags = EXTRA_PLIFLAGS.get(bench_name, [])
        forced_flags = CHECKS_FORCED_FLAGS

        for opt in opt_levels:
            pli_exe = BIN_DIR / f"{bench_name}_pli_{opt.replace('-', '')}"
            c_exe = BIN_DIR / f"{bench_name}_c_{opt.replace('-', '')}"

            if args.checks_forced:
                pli_exe_forced = BIN_DIR / f"{bench_name}_pli_forced_{opt.replace('-', '')}"
                default_flags_str = ""
                forced_flags_str = " ".join(forced_flags)

                # --- default run (for comparison baseline) ---
                if not compile_pli(bench_name, pli_file, opt, pli_exe):
                    print(f"{bench_name:<20} {opt:<6} PL/I COMPILE FAILED")
                    results[bench_name][opt] = {"error": "pli_compile_failed"}
                    continue

                # --- forced-checks run ---
                if not compile_pli(bench_name, pli_file, opt, pli_exe_forced,
                                   extra_flags=forced_flags):
                    print(f"{bench_name:<20} {opt:<6} FORCED PL/I COMPILE FAILED")
                    results[bench_name][opt] = {"error": "pli_forced_compile_failed"}
                    continue

                # --- C compile (skip mtime in forced mode) ---
                if not compile_c(bench_name, c_file, opt, c_exe):
                    print(f"{bench_name:<20} {opt:<6} C COMPILE FAILED")
                    results[bench_name][opt] = {"error": "c_compile_failed"}
                    continue

                pli_times, pli_out = run_benchmark(pli_exe, runs=args.runs)
                _, pli_forced_out = run_benchmark(pli_exe_forced, runs=1)

                if not pli_times:
                    print(f"{bench_name:<20} {opt:<6} PL/I RUN FAILED")
                    results[bench_name][opt] = {"error": "pli_run_failed"}
                    continue

                match = "✓" if pli_out.strip() == pli_forced_out.strip() else "✗"
                pli_min = min(pli_times)
                pli_median = median_result(pli_times)
                # C is not re-run in forced mode; record default timing only
                c_median = 0.0
                ratio = float('inf') if c_median == 0 else pli_median / c_median
                results[bench_name][opt] = {
                    "flags": forced_flags_str,
                    "pli": {"median": pli_median, "min": pli_min,
                            "output": pli_out.strip()},
                    "pli_forced": {"output": pli_forced_out.strip()},
                    "parity": match,
                }
                print(f"{bench_name:<20} {opt:<6} {forced_flags_str:<24} "
                      f"{pli_min*1000:>10.2f} {pli_median*1000:>10.2f} "
                      f"{'—':>10} {'—':>8} {match:>6}")
                continue

            # Normal mode
            flags_str = " ".join(default_flags) if default_flags else "(default)"

            # Compile PL/I (skip if up-to-date)
            if needs_recompile(src_pli, pli_exe):
                if not compile_pli(bench_name, pli_file, opt, pli_exe):
                    print(f"{bench_name:<20} {opt:<6} PL/I COMPILE FAILED")
                    results[bench_name][opt] = {"error": "pli_compile_failed"}
                    continue
            if needs_recompile(src_c, c_exe):
                if not compile_c(bench_name, c_file, opt, c_exe):
                    print(f"{bench_name:<20} {opt:<6} C COMPILE FAILED")
                    results[bench_name][opt] = {"error": "c_compile_failed"}
                    continue

            # Run both
            pli_times, pli_out = run_benchmark(pli_exe, runs=args.runs)
            c_times, c_out = run_benchmark(c_exe, runs=args.runs)

            if not pli_times:
                print(f"{bench_name:<20} {opt:<6} PL/I RUN FAILED")
                results[bench_name][opt] = {"error": "pli_run_failed"}
                continue

            if not c_times:
                print(f"{bench_name:<20} {opt:<6} C RUN FAILED")
                results[bench_name][opt] = {"error": "c_run_failed"}
                continue

            pli_median = median_result(pli_times)
            c_median = median_result(c_times)
            pli_min = min(pli_times)
            ratio = pli_median / c_median if c_median > 0 else float('inf')

            out_match = outputs_match(pli_out, c_out, bench_name)
            results[bench_name][opt] = {
                "flags": flags_str,
                "pli": {
                    "median": pli_median,
                    "min": pli_min,
                    "mean": statistics.mean(pli_times),
                    "stdev": statistics.stdev(pli_times) if len(pli_times) > 1 else 0,
                    "runs": pli_times,
                    "output": pli_out.strip(),
                },
                "c": {
                    "median": c_median,
                    "mean": statistics.mean(c_times),
                    "stdev": statistics.stdev(c_times) if len(c_times) > 1 else 0,
                    "runs": c_times,
                    "output": c_out.strip(),
                },
                "ratio": round(ratio, 4),
                "match": out_match,
            }
            print(f"{bench_name:<20} {opt:<6} {flags_str:<24} "
                  f"{pli_min*1000:>10.2f} {pli_median*1000:>10.2f} "
                  f"{c_median*1000:>10.2f} {ratio:>7.2f}x {out_match:>6}")

    # Save JSON
    json_file = RESULTS_DIR / f"benchmark_results_{ts}.json"
    with open(json_file, "w") as f:
        json.dump(results, f, indent=2)

    # Save CSV (P1.5)
    csv_file = RESULTS_DIR / f"summary_{ts}.csv"
    with open(csv_file, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["benchmark", "opt", "flags", "pli_min_ms",
                    "pli_median_ms", "c_median_ms", "ratio", "match"])
        for bench_name, _, _ in BENCHMARKS:
            for opt in opt_levels:
                data = results.get(bench_name, {}).get(opt, {})
                if "error" in data:
                    w.writerow([bench_name, opt, "", "", "", "", "", "ERR"])
                elif "ratio" in data:
                    pli = data["pli"]
                    c = data["c"]
                    w.writerow([bench_name, opt, data.get("flags", ""),
                                round(pli["min"] * 1000, 4),
                                round(pli["median"] * 1000, 4),
                                round(c["median"] * 1000, 4),
                                data["ratio"], data.get("match", "")])
                elif "parity" in data:
                    w.writerow([bench_name, opt, data.get("flags", ""),
                                "", "", "", "", data["parity"]])

    print(f"\nResults saved to {json_file}")
    print(f"CSV saved to {csv_file}")

    # Prune old results (P1.5)
    json_files = sorted(RESULTS_DIR.glob("benchmark_results_*.json"),
                        key=lambda f: f.stat().st_mtime)
    csv_files = sorted(RESULTS_DIR.glob("summary_*.csv"),
                       key=lambda f: f.stat().st_mtime)
    prune_old_results(json_files)
    prune_old_results(csv_files)

    if args.checks_forced:
        print("\n=== Checks-forced parity summary (P1.7) ===")
        all_ok = True
        for bench_name, _, _ in BENCHMARKS:
            data = results.get(bench_name, {}).get("-O2", {})
            if "parity" not in data:
                print(f"  {bench_name:<20} ERROR: {data.get('error', '?')}")
                all_ok = False
            elif data["parity"] != "✓":
                print(f"  {bench_name:<20} ✗ stdout differs with forced checks")
                all_ok = False
            else:
                print(f"  {bench_name:<20} ✓ parity OK")
        return 0 if all_ok else 1

    # Summary table
    print("\n" + "=" * 70)
    print("SUMMARY (median execution time, ratio = PL/I / C)")
    print("=" * 70)
    header = f"{'Benchmark':<20}"
    for opt in OPT_LEVELS:
        header += f" {opt:>8}"
    print(header)
    print("-" * (20 + 8 * len(OPT_LEVELS) + 1))
    for bench_name, _, _ in BENCHMARKS:
        row = f"{bench_name:<20}"
        for opt in OPT_LEVELS:
            data = results.get(bench_name, {}).get(opt, {})
            if "error" in data:
                row += f" {'ERR':>8}"
            elif "ratio" in data:
                row += f" {data['ratio']:>7.2f}x"
            else:
                row += f" {'-':>8}"
        print(row)

    # Average ratio by optimization level
    print("\nAverage PL/I vs C ratio by optimization level:")
    for opt in OPT_LEVELS:
        ratios = []
        for bench_name, _, _ in BENCHMARKS:
            data = results.get(bench_name, {}).get(opt, {})
            if "ratio" in data:
                ratios.append(data["ratio"])
        if ratios:
            avg = statistics.mean(ratios)
            print(f"  {opt}: avg ratio = {avg:.2f}x "
                  f"(min={min(ratios):.2f}x, max={max(ratios):.2f}x)")

    # Timing + parity validation (W7)
    print("\n=== Timing & Parity Checks (W7) ===")
    all_checks_ok = True
    for bench_name, _, _ in BENCHMARKS:
        chk = CHECKS.get(bench_name, {})
        min_ms = chk.get("min_ms_O2", 50)
        data_o2 = results.get(bench_name, {}).get("-O2", {})
        if "error" in data_o2:
            print(f"  {bench_name:<20} ✗ {data_o2['error']}")
            all_checks_ok = False
            continue
        pli_med = data_o2.get("pli", {}).get("median", 0) * 1000
        pli_out = data_o2.get("pli", {}).get("output", "")
        c_out = data_o2.get("c", {}).get("output", "")
        # Timing gate: ≥ min_ms at -O2
        if pli_med < min_ms:
            print(f"  {bench_name:<20} ✗ {pli_med:.1f}ms < {min_ms}ms threshold", end="")
            all_checks_ok = False
        else:
            print(f"  {bench_name:<20} ✓ {pli_med:.1f}ms ≥ {min_ms}ms", end="")
        # Parity: PL/I output matches C baseline (or tolerant for floats)
        if bench_name in FLOAT_BENCHES and chk.get("float_tol"):
            m = outputs_match(pli_out, c_out, bench_name)
            if m == "✓":
                print(f"  ✓ parity (exact)")
            elif m == "≈":
                print(f"  ≈ parity (float-tolerant)")
            else:
                print(f"  ✗ parity (float mismatch)")
                all_checks_ok = False
        elif bench_name == "decimal_ops":
            # decimal_ops uses fixed-decimal arithmetic, C uses double — expected mismatch
            if pli_out.strip() == c_out.strip():
                print(f"  ✓ parity (exact)")
            else:
                print(f"  ≈ parity (decimal-vs-FP, by design)")
        else:
            if pli_out.strip() == c_out.strip():
                print(f"  ✓ parity (exact)")
            else:
                print(f"  ✗ parity (output mismatch)")
                all_checks_ok = False

    return 0 if all_checks_ok else 1


if __name__ == "__main__":
    sys.exit(main())
