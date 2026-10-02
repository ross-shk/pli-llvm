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
import statistics
from pathlib import Path
from typing import Dict, List, Tuple, Optional

BENCHMARK_DIR = Path(__file__).parent
PLIC = BENCHMARK_DIR.parent / "build" / "plic"
CC = os.environ.get("CC", "cc")
RESULTS_DIR = BENCHMARK_DIR / "results"
RESULTS_DIR.mkdir(exist_ok=True)

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
]

OPT_LEVELS = ["-O0", "-O1", "-O2", "-O3"]
RUNS_PER_BENCH = 5

BENCHMARKS_REQUIRING_MATH = {"math_functions", "matrix_float"}


def run_command(cmd: List[str], cwd: Path = None, timeout: int = 300) -> Tuple[int, str, str]:
    """Run a command and return (returncode, stdout, stderr)."""
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd, timeout=timeout)
        return result.returncode, result.stdout, result.stderr
    except subprocess.TimeoutExpired:
        return -1, "", "Timeout"


def compile_pli(bench_name: str, pli_file: str, opt: str, output: Path) -> bool:
    """Compile a PL/I benchmark with plic at the given optimization level."""
    src = BENCHMARK_DIR / pli_file
    cmd = [str(PLIC), str(src), "-o", str(output), opt]
    rc, out, err = run_command(cmd)
    return rc == 0


def compile_c(bench_name: str, c_file: str, opt: str, output: Path) -> bool:
    """Compile a C benchmark with cc at the given optimization level."""
    src = BENCHMARK_DIR / c_file
    cmd = [CC, opt, str(src), "-o", str(output)]
    if bench_name in BENCHMARKS_REQUIRING_MATH:
        cmd.append("-lm")
    rc, out, err = run_command(cmd)
    return rc == 0


def run_benchmark(exe: Path, runs: int = RUNS_PER_BENCH) -> List[float]:
    """Run a benchmark multiple times and return execution times in seconds."""
    times = []
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


def main():
    print("PL/I Compiler Benchmark Harness")
    print("=" * 70)

    if not PLIC.exists():
        print(f"Error: plic not found at {PLIC}")
        print("Build first: cmake -G Ninja -S . -B build/cmake && "
              "cmake --build build/cmake -j$(nproc)")
        return 1

    # Header for the comparison table
    print(f"{'Benchmark':<20} {'Opt':<6} {'PL/I (ms)':>12} {'C (ms)':>12} "
          f"{'Ratio':>8} {'Match':>6}")
    print("-" * 68)

    results = {}

    for bench_name, pli_file, c_file in BENCHMARKS:
        results[bench_name] = {}

        for opt in OPT_LEVELS:
            pli_exe = RESULTS_DIR / f"{bench_name}_pli_{opt.replace('-', '')}"
            c_exe = RESULTS_DIR / f"{bench_name}_c_{opt.replace('-', '')}"

            # Compile PL/I
            if not compile_pli(bench_name, pli_file, opt, pli_exe):
                print(f"{bench_name:<20} {opt:<6} PL/I COMPILE FAILED")
                results[bench_name][opt] = {"error": "pli_compile_failed"}
                continue

            # Compile C
            if not compile_c(bench_name, c_file, opt, c_exe):
                print(f"{bench_name:<20} {opt:<6} C COMPILE FAILED")
                results[bench_name][opt] = {"error": "c_compile_failed"}
                continue

            # Run both
            pli_times, pli_out = run_benchmark(pli_exe)
            c_times, c_out = run_benchmark(c_exe)

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
            ratio = pli_median / c_median if c_median > 0 else float('inf')

            pli_stdev = statistics.stdev(pli_times) if len(pli_times) > 1 else 0
            c_stdev = statistics.stdev(c_times) if len(c_times) > 1 else 0

            results[bench_name][opt] = {
                "pli": {
                    "median": pli_median,
                    "mean": statistics.mean(pli_times),
                    "stdev": pli_stdev,
                    "runs": pli_times,
                    "output": pli_out.strip(),
                },
                "c": {
                    "median": c_median,
                    "mean": statistics.mean(c_times),
                    "stdev": c_stdev,
                    "runs": c_times,
                    "output": c_out.strip(),
                },
                "ratio": round(ratio, 4),
            }

            out_match = "✓" if pli_out.strip() == c_out.strip() else "✗"
            print(f"{bench_name:<20} {opt:<6} "
                  f"{pli_median*1000:>12.4f} {c_median*1000:>12.4f} "
                  f"{ratio:>7.2f}x {out_match:>6}")

    # Save results
    output_file = RESULTS_DIR / f"benchmark_results_{int(time.time())}.json"
    with open(output_file, "w") as f:
        json.dump(results, f, indent=2)
    print(f"\nResults saved to {output_file}")

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

    return 0


if __name__ == "__main__":
    sys.exit(main())
