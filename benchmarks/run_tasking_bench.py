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

What is measured: the wall time of the whole 20 000-task program, including
process startup and the final print. `ns/task` is that time divided by N, so
on these short programs it is a derived quantity, not an isolated task cost;
the README explains the noise this implies.

The gate: fiber-mode PL/I must be at least 10 % faster than Go on the task
lifecycle (pli_median / go_median <= 0.90). Go is skipped, not failed, when
the toolchain is absent. The `os` model is reported for reference and is
expected to be far slower than Go — it is the model ADR-234 replaced.

A run is admitted only if the process exited 0 and printed `done`: a crashed
or truncated workload would otherwise be timed as a very fast result.

Usage:
    python benchmarks/run_tasking_bench.py [--runs 9] [--models fiber,os]

Results are saved to benchmarks/results/tasking_<run id>.json.
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
import uuid
from pathlib import Path

from bench_common import MeasureError, probe_pinning, run_measured, sha256_file

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
TASKING = HERE / "tasking"
RESULTS_DIR = HERE / "results"
RESULTS_DIR.mkdir(exist_ok=True)
N = 20000
WARMUP = 1
EXPECTED_STDOUT = "done"

# Human-readable label for each tasking model.
MODEL_LABELS = {
    "fiber": "PL/I (fibers)",
    "os": "PL/I (OS threads)",
}

PINNING: dict = {"mode": "unprobed", "prefix": [], "applied": False}


def _find_pli() -> Path:
    """The pli binary: the newest of build/pli[.exe] and build/cmake/pli[.exe]."""
    cands = [REPO / "build" / ("pli.exe" if os.name == "nt" else "pli"),
             REPO / "build" / "cmake" / ("pli.exe" if os.name == "nt" else "pli")]
    cands = [c for c in cands if c.is_file()]
    if not cands:
        sys.exit("run_tasking_bench: no pli binary found (build it first)")
    return max(cands, key=lambda p: p.stat().st_mtime)


def _median_time(cmd, env, runs, expected_stdout):
    """Median wall time of *cmd*, with every run validated.

    Returns (median, min, samples). Raises MeasureError for the first run that
    fails, times out or prints something other than *expected_stdout*.
    """
    for _ in range(WARMUP):
        run_measured(cmd, env=env, expected_stdout=expected_stdout,
                     prefix=PINNING["prefix"])
    samples = []
    for _ in range(runs):
        seconds, _ = run_measured(cmd, env=env, expected_stdout=expected_stdout,
                                  prefix=PINNING["prefix"])
        samples.append(seconds)
    return statistics.median(samples), min(samples), samples


def _build(argv) -> str:
    """Build one baseline; return a status string ('ok' or an error)."""
    try:
        proc = subprocess.run(argv, capture_output=True, text=True,
                              encoding="utf-8")
    except OSError as exc:
        return f"spawn_failed: {exc}"
    if proc.returncode != 0:
        detail = (proc.stderr or proc.stdout or "").strip().splitlines()
        return f"build_failed: {detail[-1] if detail else proc.returncode}"
    return "ok"


def _run_side(build_status, cmd, runs, env):
    """Time one side, or record why it has no admissible measurement."""
    if build_status != "ok":
        return {"status": build_status}
    try:
        med, low, samples = _median_time(cmd, env, runs, EXPECTED_STDOUT)
    except MeasureError as exc:
        return {"status": exc.status, "detail": exc.detail}
    return {"status": "ok", "median": med, "min": low, "samples": samples}


def _ratio(pli: dict, base: dict):
    """(median ratio, min ratio), or (None, None) when a side is invalid."""
    if pli.get("status") != "ok" or base.get("status") != "ok":
        return None, None
    if base["median"] <= 0 or base["min"] <= 0:
        return None, None
    return round(pli["median"] / base["median"], 3), \
        round(pli["min"] / base["min"], 3)


def _run_workload(name, pli, runs, go, cc, model, baselines):
    """Run one workload under one model on all three sides.

    *baselines* caches the C and Go measurements: neither depends on the PL/I
    tasking model, so both models are priced against the same baseline run.
    """
    work = RESULTS_DIR / "bin"
    work.mkdir(exist_ok=True)
    exe = ".exe" if os.name == "nt" else ""

    # The model is read at runtime by rt_sched_init(); set it in the
    # execution environment for the PL/I binary. C and Go are unaffected.
    run_env = dict(os.environ)
    run_env["PLI_TASK_MODEL"] = model

    pli_exe = work / f"{name}_{model}{exe}"
    # -O2 explicitly, to match the C and Go sides rather than rely on a default.
    pli_build = _build([str(pli), str(TASKING / f"{name}.pli"), "-O2",
                        "-o", str(pli_exe)])
    pli_side = _run_side(pli_build, [str(pli_exe)], runs, run_env)

    base = baselines.setdefault(name, {})
    if "c" not in base:
        if cc:
            c_exe = work / f"{name}_c{exe}"
            c_build = _build([cc, "-O2", str(TASKING / f"{name}_c.c"),
                              "-o", str(c_exe)])
            base["c"] = _run_side(c_build, [str(c_exe)], runs, run_env)
        else:
            base["c"] = {"status": "skipped"}
    if "go" not in base:
        if go:
            go_exe = work / f"{name}_go{exe}"
            go_build = _build([go, "build", "-o", str(go_exe),
                               str(TASKING / f"{name}.go")])
            base["go"] = _run_side(go_build, [str(go_exe)], runs, run_env)
        else:
            base["go"] = {"status": "skipped"}
    c_side, go_side = base["c"], base["go"]

    go_med_ratio, go_min_ratio = _ratio(pli_side, go_side)
    c_med_ratio, _ = _ratio(pli_side, c_side)

    def per_task(side, key="median"):
        if side.get("status") != "ok":
            return None
        return side[key] / N * 1e9

    result = {
        "name": name,
        "model": model,
        "label": MODEL_LABELS.get(model, f"PL/I ({model})"),
        "n": N,
        "runs": runs,
        "warmup": WARMUP,
        "pli": pli_side,
        "c": c_side,
        "go": go_side,
        # Primary measure: the whole program's wall time. ns/task is derived
        # from it and includes process startup.
        "pli_ms": pli_side["median"] * 1e3 if pli_side.get("status") == "ok" else None,
        "pli_ns_per_task": per_task(pli_side),
        "pli_min_ns_per_task": per_task(pli_side, "min"),
        "c_ms": c_side["median"] * 1e3 if c_side.get("status") == "ok" else None,
        "c_ns_per_task": per_task(c_side),
        "go_ms": go_side["median"] * 1e3 if go_side.get("status") == "ok" else None,
        "go_ns_per_task": per_task(go_side),
        "pli_go_ratio": go_med_ratio,
        "pli_go_min_ratio": go_min_ratio,
        "pli_c_ratio": c_med_ratio,
    }
    return result


def _side_line(label, side, go_available):
    """One baseline line: wall ms and the derived ns/task, or its status."""
    if side.get("status") == "skipped":
        reason = "skipped (no go toolchain)" if not go_available else "skipped"
        return f"  {label:<18} {reason}"
    if side.get("status") != "ok":
        return f"  {label:<18} {side.get('status', 'missing')}"
    return (f"  {label:<18} {side['median']*1e3:9.3f} ms  "
            f"{side['median']/N*1e9:8.1f} ns/task")


def _print_detailed(r, go_available):
    """Print the per-workload line-by-line breakdown for one model."""
    print(f"\n{r['name']}: {N} tasks, {r['runs']} runs, model={r['model']}")
    print(_side_line(r["label"], r["pli"], go_available))
    print(_side_line("C (OS threads)", r["c"], go_available))
    if go_available:
        print(_side_line("Go (goroutines)", r["go"], go_available))
        if r.get("pli_go_ratio") is not None:
            print(f"  ratio PL/I / Go = {r['pli_go_ratio']:.3f}  "
                  f"(fiber diagnostic: <= 0.90x, startup-sensitive)")
        else:
            print(f"  ratio PL/I / Go = n/a (go side: {r['go'].get('status')})")
    else:
        print("  Go              skipped (no go toolchain) - the gate is not checked")


def _save(payload, run_id):
    """Write the result file atomically, so an interrupted run leaves none."""
    path = RESULTS_DIR / f"tasking_{run_id}.json"
    tmp = path.with_suffix(".json.tmp")
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(payload, fh, indent=2, ensure_ascii=False)
    os.replace(tmp, path)
    return path


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
                    help="report the numbers without failing on the fiber gate")
    args = ap.parse_args()

    if args.runs < 1:
        ap.error("--runs must be at least 1")
    models = [m.strip() for m in (args.task_model or args.models).split(",")
              if m.strip()]
    if not models:
        ap.error("--models needs at least one model")
    for m in models:
        if m not in ("fiber", "os"):
            ap.error(f"unknown tasking model '{m}' (choices: fiber, os)")

    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

    pli = _find_pli()
    go = shutil.which("go")
    cc = shutil.which("clang") or shutil.which("cc")

    global PINNING
    PINNING = probe_pinning()
    print(f"tasking benchmark: PL/I model(s) = {', '.join(models)}, "
          f"{args.runs} runs, median")
    print(f"pinning: {PINNING['mode']} — {PINNING['note']}\n")

    all_results = []
    ratios = {}
    baselines: dict = {}
    for model in models:
        for name in ("task_loop", "task_spawn"):
            r = _run_workload(name, pli, args.runs, go, cc, model, baselines)
            all_results.append(r)
            ratios[f"{model}_{name}"] = r.get("pli_go_ratio")
            _print_detailed(r, bool(go))

    # Combined per-workload timing table across all models.
    print("\n" + "=" * 74)
    print("SUMMARY: wall ms of the whole program (median of %d runs, "
          "%d tasks)" % (args.runs, N))
    print("=" * 74)
    header = "%-28s" % "Workload"
    for m in models:
        header += "%16s" % (m + " ms")
    header += "%16s%16s" % ("Go", "C (os-thr)")
    print(header)
    print("-" * len(header))
    for name in ("task_loop", "task_spawn"):
        row = "%-28s" % name
        for m in models:
            r = next((x for x in all_results
                      if x["model"] == m and x["name"] == name), None)
            row += "%16s" % (("%.3f" % r["pli_ms"]) if r and r.get("pli_ms") is not None
                             else (r["pli"].get("status", "—") if r else "—"))
        go_r = next((x for x in all_results if x["name"] == name), None)
        row += "%16s" % (("%.3f" % go_r["go_ms"]) if go_r and go_r.get("go_ms") is not None
                         else "—")
        row += "%16s" % (("%.3f" % go_r["c_ms"]) if go_r and go_r.get("c_ms") is not None
                         else "—")
        print(row)

    # PL/I vs Go ratio table (the key deliverable).
    print("\nPL/I vs Go ratio (median PL/I / median Go):")
    print("-" * 52)
    print("%-16s%16s%16s" % ("Model", "task_loop", "task_spawn"))
    print("-" * 52)
    for m in models:
        row = "%-16s" % m
        for name in ("task_loop", "task_spawn"):
            ratio = ratios.get(f"{m}_{name}")
            row += ("%15.3fx" % ratio) if ratio is not None else "%16s" % "n/a"
        print(row)

    # PL/I vs C ratio (only meaningful for the os model, but shown for both).
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
                    row += "%16s" % "n/a"
            print(row)

    # Save JSON with machine metadata and the manifest.
    run_id = f"{int(time.time())}_{uuid.uuid4().hex[:8]}"
    machine_info = {
        "python": sys.version.split()[0],
        "platform": platform.platform(aliased=True),
        "machine": platform.machine(),
        "cpu_count": os.cpu_count(),
        "models": models,
        "n": N,
        "runs": args.runs,
        "warmup": WARMUP,
        "pinning": PINNING,
        "baselines": "C and Go measured once per workload, shared across models",
        "pli_binary": str(pli),
        "pli_sha256": sha256_file(pli),
        "go": go,
        "cc": cc,
        "sources": {src: sha256_file(TASKING / src) for src in (
            "task_loop.pli", "task_spawn.pli", "task_loop_c.c",
            "task_spawn_c.c", "task_loop.go", "task_spawn.go")},
    }
    json_file = _save({"schema": 2, "run_id": run_id, "machine": machine_info,
                       "results": all_results}, run_id)
    print(f"\nResults saved to {json_file}")

    # Any failed side is a failure of the run: a workload that crashed was
    # not measured, whatever its wall time looked like.
    failed = [(f"{r['model']}/{r['name']}", side)
              for r in all_results
              for side in ("pli", "c", "go")
              if r[side].get("status") not in ("ok", "skipped")]
    if failed:
        print("\nFAIL: invalid measurement(s):")
        for key, side in failed:
            print(f"  {key}: {side.get('status')} {side.get('detail', '')}".rstrip())
        return 1

    # Gate check: fiber only, and only when Go produced a valid measurement.
    if "fiber" in models and go:
        gate = ratios.get("fiber_task_loop")
        if gate is None:
            print("the fiber gate is not checked (no valid Go measurement)")
        elif gate > 0.90:
            print(f"\nFAIL: fiber-mode PL/I is {gate:.3f}x Go's time on the "
                  f"task lifecycle — the gate is <= 0.90x")
            # --no-gate reports the number without failing the run.
            return 0 if args.no_gate else 1
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
