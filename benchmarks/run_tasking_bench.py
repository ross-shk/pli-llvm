#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
"""
Tasking benchmark harness (ADR-234).

Compares PL/I's tasking model with Go's goroutines on the lightest possible
task. Two workloads:

  W1  fan-out / fan-in: start N tasks that do nothing, then join all N.
      Every task is live at once, so this measures the cost of *creating*
      the task's execution context (the fiber stack) as much as the
      tasking itself.

  W1b the task lifecycle: start a task that does nothing and join it, N
      times over. The fiber pool recycles the stack, so this measures the
      tasking model itself: the spawn, the switch and the join.

PL/I runs under one or two tasking models:

  fiber — ADR-234's Go-style lightweight threading (default). Tasks are
          fibers on a bounded worker pool.
  os    — the legacy ADR-016 model: one OS thread per task ("multithreading").

The C version (OS threads) is the reference for the `os` model. Go
(goroutines) is the universal baseline for both PL/I models.

The gate: fiber-mode PL/I must be at least 10 % faster than Go on the task
lifecycle (pli_median / go_median <= 0.90). Go is skipped, not failed, when
the toolchain is absent. The `os` model is reported for reference and is
expected to be far slower than Go — it is the model ADR-234 replaced.

Usage:
    python benchmarks/run_tasking_bench.py [--runs 9] [--models fiber,os]

Results are saved to benchmarks/results/tasking_<ts>.json.
"""
import argparse
import json
import os
import platform
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
TASKING = HERE / "tasking"
RESULTS_DIR = HERE / "results"
RESULTS_DIR.mkdir(exist_ok=True)
N = 20000

# Human-readable label for each tasking model.
MODEL_LABELS = {
    "fiber": "PL/I (fibers)",
    "os": "PL/I (OS threads)",
}


def _find_pli() -> Path:
    """The pli binary: the newest of build/pli[.exe] and build/cmake/pli[.exe]."""
    cands = [REPO / "build" / ("pli.exe" if os.name == "nt" else "pli"),
             REPO / "build" / "cmake" / ("pli.exe" if os.name == "nt" else "pli")]
    cands = [c for c in cands if c.is_file()]
    if not cands:
        sys.exit("run_tasking_bench: no pli binary found (build it first)")
    return max(cands, key=lambda p: p.stat().st_mtime)


def _pin_cmd(cmd):
    """Prepend process pinning for stable timings (same logic as run_benchmarks.py)."""
    if os.name == "nt":
        return cmd
    if platform.system() == "Darwin":
        return ["nice", "-n", "-20"] + cmd
    if shutil.which("taskset"):
        return ["taskset", "-c", "0"] + cmd
    return cmd


def _median_time(cmd, env, runs, warmup=1):
    """The median wall time of `cmd`, in seconds."""
    for _ in range(warmup):
        subprocess.run(_pin_cmd(cmd), env=env, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, check=False)
    times = []
    for _ in range(runs):
        start = time.perf_counter()
        subprocess.run(_pin_cmd(cmd), env=env, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, check=False)
        times.append(time.perf_counter() - start)
    return statistics.median(times), min(times)


def _run_workload(name, pli, runs, go, cc, model):
    """Run one workload under one model on all three sides.

    Returns a dict with per-side timings (seconds) and derived ratios.
    """
    work = RESULTS_DIR / "bin"
    work.mkdir(exist_ok=True)
    exe = ".exe" if os.name == "nt" else ""
    pli_label = MODEL_LABELS.get(model, f"PL/I ({model})")

    # The model is read at runtime by rt_sched_init(); set it in the
    # execution environment for the PL/I binary. C and Go are unaffected.
    run_env = dict(os.environ)
    run_env["PLI_TASK_MODEL"] = model

    pli_exe = work / f"{name}_{model}{exe}"
    subprocess.run([str(pli), str(TASKING / f"{name}.pli"), "-o", str(pli_exe)],
                   cwd=REPO, check=True)
    pli_med, pli_min = _median_time([str(pli_exe)], run_env, runs)

    c_med = c_min = None
    if cc:
        c_exe = work / f"{name}_c{exe}"
        build = subprocess.run([cc, "-O2", str(TASKING / f"{name}_c.c"),
                                "-o", str(c_exe)], capture_output=True, text=True)
        if build.returncode == 0:
            c_med, c_min = _median_time([str(c_exe)], run_env, runs)
        else:
            print(f"  C reference: build failed\n{build.stderr}")

    go_med = go_min = None
    if go:
        go_exe = work / f"{name}_go{exe}"
        build = subprocess.run([go, "build", "-o", str(go_exe),
                                str(TASKING / f"{name}.go")],
                               capture_output=True, text=True)
        if build.returncode == 0:
            go_med, go_min = _median_time([str(go_exe)], run_env, runs)
        else:
            print(f"  go: build failed, skipping the baseline\n{build.stderr}")

    result = {
        "name": name,
        "model": model,
        "label": pli_label,
        "n": N,
        "runs": runs,
        "pli_ns": pli_med / N * 1e9,
        "pli_ms": pli_med * 1e3,
        "pli_min_ns": pli_min / N * 1e9,
        "c_ns": c_med / N * 1e9 if c_med else None,
        "c_ms": c_med * 1e3 if c_med else None,
        "go_ns": go_med / N * 1e9 if go_med else None,
        "go_ms": go_med * 1e3 if go_med else None,
    }
    if go_med:
        ratio_med = pli_med / go_med
        ratio_min = pli_min / go_min if go_min else None
        result["pli_go_ratio"] = round(ratio_med, 3)
        result["pli_go_min_ratio"] = round(ratio_min, 3) if ratio_min else None
    if c_med:
        result["pli_c_ratio"] = round(pli_med / c_med, 3)
    return result


def _print_detailed(r, go_avail):
    """Print the per-workload line-by-line breakdown for one model."""
    print(f"\n{r['name']}: {N} tasks, {r['runs']} runs, model={r['model']}")
    print(f"  {r['label']:<18} {r['pli_ms']:9.3f} ms  {r['pli_ns']:8.1f} ns/task")
    if r["c_ms"] is not None:
        print(f"  C (OS threads)  {r['c_ms']:9.3f} ms  {r['c_ns']:8.1f} ns/task")
    if go_avail:
        print(f"  Go (goroutines) {r['go_ms']:9.3f} ms  {r['go_ns']:8.1f} ns/task")
        print(f"  ratio PL/I / Go = {r['pli_go_ratio']:.3f}  "
              f"(gate for fiber <= 0.90x)")
    else:
        print("  Go              skipped (no go toolchain) - the gate is not checked")


def main() -> int:
    ap = argparse.ArgumentParser(description="Tasking benchmark harness")
    ap.add_argument("--runs", type=int, default=9,
                    help="timed runs per side (default: %(default)s)")
    ap.add_argument("--models", type=str, default="fiber",
                    help="comma-separated PL/I tasking models to benchmark "
                         "(fiber, os); default: fiber")
    ap.add_argument("--task-model", type=str, default=None,
                    help="(deprecated alias for --models)")
    ap.add_argument("--no-gate", action="store_true",
                    help="report the numbers without asserting the gate")
    args = ap.parse_args()

    if args.task_model:
        models = args.task_model
    else:
        models = args.models
    models = [m.strip() for m in models.split(",") if m.strip()]

    for m in models:
        if m not in ("fiber", "os"):
            ap.error(f"unknown tasking model '{m}' (choices: fiber, os)")

    pli = _find_pli()
    go = shutil.which("go")
    cc = shutil.which("clang") or shutil.which("cc")

    model_str = ", ".join(models)
    print(f"tasking benchmark: PL/I model(s) = {model_str}, "
          f"{args.runs} runs, median\n")

    all_results = []
    ratios = {}
    for model in models:
        for name in ("task_loop", "task_spawn"):
            r = _run_workload(name, pli, args.runs, go, cc, model)
            all_results.append(r)
            ratios[f"{model}_{name}"] = r.get("pli_go_ratio")
            _print_detailed(r, bool(go))

    # Combined per-workload timing table across all models.
    print("\n" + "=" * 74)
    print("SUMMARY: ns/task (median of %d runs, %d tasks)" % (args.runs, N))
    print("=" * 74)
    header = "%-28s" % "Workload"
    for m in models:
        header += "%16s" % (m + " ns/task")
    header += "%16s%16s" % ("Go", "C (os-thr)")
    print(header)
    print("-" * len(header))
    for name in ("task_loop", "task_spawn"):
        row = "%-28s" % name
        for m in models:
            r = next((x for x in all_results
                      if x["model"] == m and x["name"] == name), None)
            row += "%16.1f" % r["pli_ns"] if r else "%16s" % "—"
        go_r = next((x for x in all_results if x["name"] == name), None)
        row += "%16.1f" % go_r["go_ns"] if go_r and go_r["go_ns"] else "%16s" % "—"
        row += "%16.1f" % go_r["c_ns"] if go_r and go_r["c_ns"] else "%16s" % "—"
        print(row)

    # PL/I vs Go ratio table (the key deliverable).
    print("\nPL/I vs Go ratio (median PL/I / median Go):")
    print("-" * 52)
    print("%-16s%16s%16s" % ("Model", "task_loop", "task_spawn"))
    print("-" * 52)
    for m in models:
        row = "%-16s" % m
        for name in ("task_loop", "task_spawn"):
            r = next((x for x in all_results
                      if x["model"] == m and x["name"] == name), None)
            if r and r.get("pli_go_ratio") is not None:
                row += "%15.3fx" % r["pli_go_ratio"]
            else:
                row += "%16s" % "—"
        print(row)

    # PL/I vs C ratio (only meaningful for os model, but show for both).
    if any(r.get("pli_c_ratio") is not None for r in all_results):
        print("\nPL/I vs C ratio (median PL/I / median C, OS-threads baseline):")
        print("-" * 52)
        print("%-16s%16s%16s" % ("Model", "task_loop", "task_spawn"))
        print("-" * 52)
        for m in models:
            row = "%-16s" % m
            for name in ("task_loop", "task_spawn"):
                r = next((x for x in all_results
                          if x["model"] == m and x["name"] == name), None)
                if r and r.get("pli_c_ratio") is not None:
                    row += "%15.3fx" % r["pli_c_ratio"]
                else:
                    row += "%16s" % "—"
            print(row)

    # Save JSON.
    ts = int(time.time())
    machine_info = {
        "python": sys.version.split()[0],
        "platform": platform.platform(aliased=True),
        "machine": platform.machine(),
        "cpu_count": os.cpu_count(),
        "models": models,
    }
    json_file = RESULTS_DIR / f"tasking_{ts}.json"
    with open(json_file, "w", encoding="utf-8") as f:
        json.dump({"machine": machine_info, "results": all_results}, f,
                  indent=2, ensure_ascii=False)
    print(f"\nResults saved to {json_file}")

    # Gate check: only applies to the fiber model.
    if "fiber" in models and go:
        gate = ratios.get("fiber_task_loop")
        if gate is None:
            print("the fiber gate is not checked (Go unavailable)")
        elif gate > 0.90:
            print(f"\nFAIL: fiber-mode PL/I is {gate:.3f}x Go's time on the "
                  f"task lifecycle — the gate is <= 0.90x")
            return 1 if not args.no_gate else 0
        else:
            print(f"\nPASS: the fiber tasking gate holds ({gate:.3f}x <= 0.90x)")

    if "os" in models and go:
        gate_os = ratios.get("os_task_loop")
        if gate_os is not None:
            print(f"\nNote: the 'os' (OS-thread / multithreading) model is "
                  f"{gate_os:.3f}x Go's time — this is the reference model "
                  f"ADR-234 replaced, not a gate.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
