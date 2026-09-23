#!/usr/bin/env python3
"""
PL/I Compiler Benchmark Harness
Compiles and runs benchmarks with different optimization levels, comparing against C baselines.
"""
import os
import subprocess
import sys
import time
import json
import statistics
from pathlib import Path
from typing import Dict, List, Tuple

BENCHMARK_DIR = Path(__file__).parent
PLIC = BENCHMARK_DIR.parent / "build" / "plic"
RESULTS_DIR = BENCHMARK_DIR / "results"
RESULTS_DIR.mkdir(exist_ok=True)

BENCHMARKS = [
    "scalar_arith.pli",
    "array_matmul.pli", 
    "string_ops.pli",
    "decimal_ops.pli",
    "condition_ops.pli",
    "proc_calls.pli",
]

OPT_LEVELS = ["-O0", "-O1", "-O2", "-O3"]

def run_command(cmd: List[str], cwd: Path = None, timeout: int = 300) -> Tuple[int, str, str]:
    """Run a command and return (returncode, stdout, stderr)."""
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd, timeout=timeout)
        return result.returncode, result.stdout, result.stderr
    except subprocess.TimeoutExpired:
        return -1, "", "Timeout"

def compile_benchmark(bench: str, opt: str, output: Path) -> bool:
    """Compile a benchmark with given optimization level."""
    src = BENCHMARK_DIR / bench
    cmd = [str(PLIC), str(src), "-o", str(output), opt]
    rc, out, err = run_command(cmd)
    return rc == 0

def run_benchmark(exe: Path, runs: int = 10) -> List[float]:
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
            return []
    return times

def create_c_baseline(bench: str) -> Path:
    """Create a C equivalent of the benchmark for comparison."""
    # This would need manual C translations for each benchmark
    # For now, return None - we'll implement this later
    return None

def main():
    print("PL/I Compiler Benchmark Harness")
    print("=" * 50)
    
    if not PLIC.exists():
        print(f"Error: plic not found at {PLIC}")
        print("Run 'make -j8' first")
        return 1
    
    results = {}
    
    for bench in BENCHMARKS:
        print(f"\nBenchmark: {bench}")
        bench_name = bench.replace(".pli", "")
        results[bench_name] = {}
        
        for opt in OPT_LEVELS:
            print(f"  {opt}...", end=" ", flush=True)
            exe = RESULTS_DIR / f"{bench_name}_{opt.replace('-', '')}"
            
            # Compile
            if not compile_benchmark(bench, opt, exe):
                print("COMPILE FAILED")
                results[bench_name][opt] = {"error": "compile_failed"}
                continue
            
            # Run
            times = run_benchmark(exe, runs=5)
            if not times:
                print("RUN FAILED")
                results[bench_name][opt] = {"error": "run_failed"}
                continue
            
            median_time = statistics.median(times)
            mean_time = statistics.mean(times)
            stdev = statistics.stdev(times) if len(times) > 1 else 0
            
            results[bench_name][opt] = {
                "median": median_time,
                "mean": mean_time,
                "stdev": stdev,
                "runs": times
            }
            print(f"median={median_time:.4f}s mean={mean_time:.4f}s ±{stdev:.4f}s")
    
    # Save results
    output_file = RESULTS_DIR / f"benchmark_results_{int(time.time())}.json"
    with open(output_file, "w") as f:
        json.dump(results, f, indent=2)
    print(f"\nResults saved to {output_file}")
    
    # Print summary
    print("\n" + "=" * 50)
    print("SUMMARY")
    print("=" * 50)
    for bench_name, bench_results in results.items():
        print(f"\n{bench_name}:")
        for opt, data in bench_results.items():
            if "error" in data:
                print(f"  {opt}: {data['error']}")
            else:
                print(f"  {opt}: {data['median']:.4f}s (median)")

if __name__ == "__main__":
    sys.exit(main())