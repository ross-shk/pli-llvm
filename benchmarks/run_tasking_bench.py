#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
"""
Tasking benchmark harness (ADR-234).

Compares PL/I's tasking model with Go's goroutines on the lightest
possible task. Two workloads:

  W1  fan-out / fan-in: start N tasks that do nothing, then join all N.
      Every task is live at once, so this measures the cost of *creating*
      the task's execution context (the fiber stack) as much as the
      tasking itself.
  W1b the task lifecycle: start a task that does nothing and join it, N
      times over. The fiber pool recycles the stack, so this measures the
      tasking model itself: the spawn, the switch and the join.

The C version (OS threads) is the reference point: it is the model
ADR-016 chose, so it shows what the fiber model buys.

The gate: PL/I must be at least 10 % faster than Go on the task
lifecycle (pli_median / go_median <= 0.90). Go is skipped, not failed,
when the toolchain is absent.

Usage:
    python benchmarks/run_tasking_bench.py [--runs 9] [--task-model fiber]
"""
import argparse
import os
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
TASKING = HERE / "tasking"
N = 20000


def _find_pli() -> Path:
    """The pli binary: the newest of build/pli[.exe] and build/cmake/pli[.exe]."""
    cands = [REPO / "build" / ("pli.exe" if os.name == "nt" else "pli"),
             REPO / "build" / "cmake" / ("pli.exe" if os.name == "nt" else "pli")]
    cands = [c for c in cands if c.is_file()]
    if not cands:
        sys.exit("run_tasking_bench: no pli binary found (build it first)")
    return max(cands, key=lambda p: p.stat().st_mtime)


def _median_time(cmd, env, runs, warmup=1):
    """The median wall time of `cmd`, in seconds."""
    for _ in range(warmup):
        subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, check=False)
    times = []
    for _ in range(runs):
        start = time.perf_counter()
        subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, check=False)
        times.append(time.perf_counter() - start)
    return statistics.median(times)


def _run_workload(name, pli, env, runs, go, cc, gate):
    """Run one workload on all three sides; return the PL/I / Go ratio."""
    work = HERE / "out"
    work.mkdir(exist_ok=True)
    exe = ".exe" if os.name == "nt" else ""

    pli_exe = work / f"{name}{exe}"
    subprocess.run([str(pli), str(TASKING / f"{name}.pli"), "-o", str(pli_exe)],
                   cwd=REPO, check=True)
    pli_t = _median_time([str(pli_exe)], env, runs)

    c_t = None
    if cc:
        c_exe = work / f"{name}_c{exe}"
        build = subprocess.run([cc, "-O2", str(TASKING / f"{name}_c.c"),
                                "-o", str(c_exe)], capture_output=True, text=True)
        if build.returncode == 0:
            c_t = _median_time([str(c_exe)], env, runs)
        else:
            print(f"C reference: build failed, skipping\n{build.stderr}")

    go_t = None
    if go:
        go_exe = work / f"{name}_go{exe}"
        build = subprocess.run([go, "build", "-o", str(go_exe),
                                str(TASKING / f"{name}.go")],
                               capture_output=True, text=True)
        if build.returncode == 0:
            go_t = _median_time([str(go_exe)], env, runs)
        else:
            print(f"go: build failed, skipping the baseline\n{build.stderr}")

    print(f"{name}: {N} tasks, {runs} runs")
    print(f"  PL/I (fibers)   {pli_t * 1e3:9.3f} ms  {pli_t / N * 1e9:8.1f} ns/task")
    if c_t is not None:
        print(f"  C (OS threads)  {c_t * 1e3:9.3f} ms  {c_t / N * 1e9:8.1f} ns/task")
    if go_t is None:
        print("  Go              skipped (no go toolchain) - the gate is not checked")
        return None
    print(f"  Go (goroutines) {go_t * 1e3:9.3f} ms  {go_t / N * 1e9:8.1f} ns/task")
    ratio = pli_t / go_t
    print(f"  ratio PL/I / Go = {ratio:.3f}  (the gate is <= 0.90)")
    return ratio


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=9)
    ap.add_argument("--task-model", default="fiber",
                    choices=["fiber", "os"],
                    help="the PL/I tasking model to benchmark (default: fiber)")
    ap.add_argument("--no-gate", action="store_true",
                    help="report the numbers without asserting the gate")
    args = ap.parse_args()

    pli = _find_pli()
    env = dict(os.environ)
    env["PLI_TASK_MODEL"] = args.task_model
    go = shutil.which("go")
    cc = shutil.which("clang") or shutil.which("cc")

    print(f"tasking benchmark: PL/I model = {args.task_model}, "
          f"{args.runs} runs, median\n")
    ratios = {}
    for name in ("task_loop", "task_spawn"):
        ratios[name] = _run_workload(name, pli, env, args.runs, go, cc, True)
        print()

    if args.no_gate:
        return 0
    gate = ratios.get("task_loop")
    if gate is None:
        print("the gate is not checked (no go toolchain)")
        return 0
    if gate > 0.90:
        print(f"FAIL: PL/I is {gate:.2f}x Go's time on the task lifecycle - "
              f"the gate is <= 0.90x")
        return 1
    print("PASS: the tasking gate holds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
