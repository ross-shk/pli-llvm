#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
"""
PL/I Compiler Benchmark Harness

Compiles and runs benchmarks with different optimization levels, comparing
generated PL/I code performance against C baselines. Each benchmark has a
`.pli` source (compiled with `pli`) and a `baseline/<name>_c.c` source
(compiled with `clang`), all implementing the same algorithm.
"""
import os
import platform
import shutil
import subprocess
import sys
import time
import json
import csv
import math
import statistics
from pathlib import Path
from typing import Dict, List, Set, Tuple, Optional

BENCHMARK_DIR = Path(__file__).parent


# Two benchmark categories (ADR-262):
#
#   ir/   the C-parity corpus. Generic workloads whose PL/I and C twins do
#         the same thing, so the ratio measures the LLVM pipeline's output
#         against equivalent C. Its geomean is the project's acceptance gate.
#
#   lang/ the PL/I-language corpus. Workloads that exercise PL/I semantics a
#         C program cannot express: range checks, ON-unit dispatch, decimal
#         scale, bit fields, constant concatenation, list-directed output,
#         aliasing. Each still has a C twin for reference, but the gates the
#         optimization plan sets on them are paired PL/I-vs-PL-I comparisons
#         (see PAIRS), because a C twin cannot price a PL/I-only cost.
IR_BENCHMARKS = [
    ("scalar_arith",      "ir/scalar_arith.pli",      "ir/baseline/scalar_arith_c.c"),
    ("array_matmul",      "ir/array_matmul.pli",      "ir/baseline/array_matmul_c.c"),
    ("string_ops",        "ir/string_ops.pli",        "ir/baseline/string_ops_c.c"),
    ("decimal_ops",       "ir/decimal_ops.pli",       "ir/baseline/decimal_ops_c.c"),
    ("condition_ops",     "ir/condition_ops.pli",     "ir/baseline/condition_ops_c.c"),
    ("proc_calls",        "ir/proc_calls.pli",        "ir/baseline/proc_calls_c.c"),
    ("math_functions",    "ir/math_functions.pli",    "ir/baseline/math_functions_c.c"),
    ("bit_ops",           "ir/bit_ops.pli",           "ir/baseline/bit_ops_c.c"),
    ("array_sort",        "ir/array_sort.pli",        "ir/baseline/array_sort_c.c"),
    ("linked_list",       "ir/linked_list.pli",       "ir/baseline/linked_list_c.c"),
    ("collatz",           "ir/collatz.pli",           "ir/baseline/collatz_c.c"),
    ("controlled_stack",  "ir/controlled_stack.pli",  "ir/baseline/controlled_stack_c.c"),
    ("on_conditions",     "ir/on_conditions.pli",     "ir/baseline/on_conditions_c.c"),
    ("matrix_float",      "ir/matrix_float.pli",      "ir/baseline/matrix_float_c.c"),
    ("mandelbrot",        "ir/mandelbrot.pli",        "ir/baseline/mandelbrot_c.c"),
    ("io_throughput",     "ir/io_throughput.pli",     "ir/baseline/io_throughput_c.c"),
    ("division",          "ir/division.pli",          "ir/baseline/division_c.c"),
    ("fixed_bin63",       "ir/fixed_bin63.pli",       "ir/baseline/fixed_bin63_c.c"),
    ("bcd_decimal",       "ir/bcd_decimal.pli",       "ir/baseline/bcd_decimal_c.c"),
    ("string_search",     "ir/string_search.pli",     "ir/baseline/string_search_c.c"),
    ("file_io",           "ir/file_io.pli",           "ir/baseline/file_io_c.c"),
]

LANG_BENCHMARKS = [
    ("checks_on",     "lang/checks_on.pli",     "lang/baseline/checks_on_c.c"),
    ("on_dispatch",   "lang/on_dispatch.pli",   "lang/baseline/on_dispatch_c.c"),
    ("decimal_scale", "lang/decimal_scale.pli", "lang/baseline/decimal_scale_c.c"),
    ("bit_fields",    "lang/bit_fields.pli",    "lang/baseline/bit_fields_c.c"),
    ("bit_merge",     "lang/bit_merge.pli",     "lang/baseline/bit_merge_c.c"),
    ("concat_const",  "lang/concat_const.pli",  "lang/baseline/concat_const_c.c"),
    ("io_list",       "lang/io_list.pli",       "lang/baseline/io_list_c.c"),
    ("alias_scope",   "lang/alias_scope.pli",   "lang/baseline/alias_scope_c.c"),
    ("pure_call",     "lang/pure_call.pli",     "lang/baseline/pure_call_c.c"),
    ("picture_fold",  "lang/picture_fold.pli",  "lang/baseline/picture_fold_c.c"),
    # WS-K5: a BY NAME structure assignment. The C twin copies the structure
    # with one aggregate move, which is what a REDUCE lowering would produce.
    ("by_name",       "lang/by_name.pli",       "lang/baseline/by_name_c.c"),
    # WS-K10: a SELECT over twenty constant CHAR arms. The C twin searches
    # the same constants with a binary search, which is what a
    # binary-search lowering would produce.
    ("select_chain",  "lang/select_chain.pli",  "lang/baseline/select_chain_c.c"),
]

# WS-A2 (ADR-267): a benchmark whose callee lives in another compilation unit
# publishes the effects that callee promises, so the caller's build can
# consume them. The value is the library source; the file is where the
# published effects land (in the results directory, not the source tree).
PUBLISH_EFFECTS = {
    "pure_call_split": ("lang/pure_call_lib.pli", "pure_call_split.eff"),
}

# Every benchmark, both categories.
BENCHMARKS = IR_BENCHMARKS + LANG_BENCHMARKS

# Two-unit benchmarks: one program split across two compilation units, so the
# caller's optimizer never sees the callee's body. WS-A1's measurement needs
# exactly that shape (docs/OPTIMIZATION-BASELINE.md §3).
MULTI_BENCHMARKS = [
    ("pure_call_split", ["lang/pure_call_split.pli", "lang/pure_call_lib.pli"]),
]

# Paired PL/I-vs-PL/I comparisons (ADR-262): the same source compiled two
# ways, so the ratio prices a PL/I-specific cost a C twin cannot show.
# Each entry is (benchmark, label_a, flags_a, label_b, flags_b); the runner
# reports the B/A median time ratio at -O2 and requires both builds to print
# the same output. An entry may add a sixth element, the sources for side B,
# when the two sides are different programs.
PAIRS = [
    # M7's criterion: check-heavy code within 1.5x of check-free code. The
    # default -O2 build elides the SIZE and SUBSCRIPTRANGE traps; the
    # "checks-on" build keeps them.
    ("checks_on", "check-free", [], "checks-on",
     ["--size-checks", "--subscript-checks"]),
    # The same pair, one check kind at a time: ADR-252 records that the
    # SUBSCRIPTRANGE checks are already free (LLVM folds them) and the
    # SIZE overflow checks are the whole difference, and these two pairs
    # are what says so.
    ("checks_on", "check-free", [], "subscript-checks",
     ["--subscript-checks"]),
    ("checks_on", "check-free", [], "size-checks",
     ["--size-checks"]),
    # WS-A1 (ADR-264): what the inferred procedure memory effects are worth.
    # The same program, once with the inference and once with
    # --no-memory-effects, so every call keeps LLVM's default effects.
    ("pure_call", "no-effects", ["--no-memory-effects"], "effects", []),
    # The same measurement with the callee in another compilation unit: the
    # caller's optimizer cannot see the callee's body, so it cannot infer the
    # callee's effects either, and the promise on the definition reaches
    # nobody. The ratio prices the inference where it cannot travel.
    ("pure_call_split", "no-effects", ["--no-memory-effects"], "effects", []),
    # WS-A2 (ADR-267): the same split program, with the callee publishing its
    # promise and the caller consuming it. This is the shape the promise was
    # for: the callee's body is invisible, so the published description is
    # all the caller's optimizer has.
    ("pure_call_split", "no-promise", [], "published", ["@effects"]),
    # What the split costs: the same program with the callee in the caller's
    # own unit is the reference, and the gap between the two is what a promise
    # that reached the caller would be worth.
    ("pure_call_split", "split", [], "same-unit", [], ["lang/pure_call.pli"]),
    # B4 (ADR-274): a constant P item through a constant picture is folded
    # to text; the other side keeps the runtime interpreter. The per-op
    # override isolates the picture lowering from the rest of the pipeline.
    ("picture_fold", "folded", [], "runtime",
     ["--experimental-lowering=picture:runtime"]),
    # WS-C3 (ADR-243): two masked bit tests on one value in one conjunction
    # merge into one masked test; the other side keeps the two tests, so the
    # pair prices the merge itself.
    ("bit_merge", "merged", [], "separate",
     ["--experimental-lowering=bit_merge:runtime"]),
    # WS-C1 (ADR-242): a concatenation with constant-length leaves is stored
    # at constant offsets; the other side keeps the temporary buffer.
    ("concat_const", "constant-offset", [], "buffer",
     ["--experimental-lowering=concat_const:runtime"]),
    # WS-K2 (ADR-254): the inline-vs-library decision. The same program on
    # the library path (--experimental-lowering=runtime) and on the inline
    # path, so the ratio prices the library call for the shapes the repo
    # supports. A ratio near 1 means the library path is competitive and
    # the decision layer has nothing to buy.
    ("string_search", "inline", [], "library",
     ["--experimental-lowering=runtime"]),
]


def _default_cc() -> str:
    """C compiler default: plain `clang` (found via PATH on every OS)."""
    found = shutil.which("clang")
    return found or "clang"


def _find_pli() -> Path:
    """Locate the pli binary: newest of `build/pli[.exe]`, `build/cmake/...`."""
    here = BENCHMARK_DIR.parent / "build"
    cands = [
        here / ("pli.exe" if os.name == "nt" else "pli"),
        here / "pli",
        here / "pli.exe",
        here / "cmake" / ("pli.exe" if os.name == "nt" else "pli"),
        here / "cmake" / "pli",
        here / "cmake" / "pli.exe",
    ]
    found = [c for c in cands if c.is_file()]
    if found:
        return max(found, key=lambda c: c.stat().st_mtime)
    return cands[0]


PLI = Path(os.environ.get("PLI", str(_find_pli())))
CC = os.environ.get("CC", _default_cc())
IS_MACOS = platform.system() == "Darwin"
RESULTS_DIR = BENCHMARK_DIR / "results"
BIN_DIR = RESULTS_DIR / "bin"
RESULTS_DIR.mkdir(exist_ok=True)
BIN_DIR.mkdir(exist_ok=True)

MAX_RESULTS = 20

# Warn if any benchmark's PL/I/C ratio at -O2 worsens by more than this relative
# threshold compared to the most recent saved run (P1.1 W7 regression guard).
DETERIORATION_THRESHOLD = 0.05

# Per-benchmark PL/I flags that must be active for the benchmark to measure
# what it claims (P1.6).
EXTRA_PLIFLAGS = {
    "condition_ops": ["--subscript-checks"],
}

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
    "fixed_bin63":      {"min_ms_O2": 50, "float_tol": False},
    "bcd_decimal":      {"min_ms_O2": 50, "float_tol": False},  # expected ~: BCD decimal vs double
    "string_search":    {"min_ms_O2": 50, "float_tol": False},
    "file_io":          {"min_ms_O2": 50, "float_tol": False},
    # The PL/I-language corpus: exact parity with the C twin (both are
    # exact integer or fixed-point arithmetic, never floating point).
    "checks_on":        {"min_ms_O2": 50, "float_tol": False},
    "on_dispatch":      {"min_ms_O2": 50, "float_tol": False},
    "decimal_scale":    {"min_ms_O2": 50, "float_tol": False},
    "bit_fields":       {"min_ms_O2": 50, "float_tol": False},
    "concat_const":     {"min_ms_O2": 50, "float_tol": False},
    "io_list":          {"min_ms_O2": 50, "float_tol": False},
    "alias_scope":      {"min_ms_O2": 50, "float_tol": False},
    "picture_fold":     {"min_ms_O2": 50, "float_tol": False},
}


def get_machine_info() -> Dict:
    """Collect machine and toolchain metadata for JSON results (P3)."""
    info = {
        "python": sys.version.split()[0],
        "platform": platform.platform(aliased=True),
        "machine": platform.machine(),
        "processor": platform.processor() or "unknown",
        "cpu_count": os.cpu_count(),
    }
    try:
        rc, out, _ = run_command([str(PLI), "--version"])
        if rc == 0:
            first = out.strip().split('\n')[0]
            info["pli_version"] = first
            # Extract LLVM version from parenthetical: "pli v... (LLVM 23.1.2, ...)"
            import re
            m = re.search(r'\(LLVM\s+([\d.]+)', first)
            info["llvm_version"] = m.group(1) if m else "unknown"
    except Exception:
        info["pli_version"] = "unknown"
        info["llvm_version"] = "unknown"
    try:
        rc, out, _ = run_command([CC, "--version"])
        if rc == 0:
            info["clang_version"] = out.strip().split('\n')[0]
    except Exception:
        info["clang_version"] = "unknown"
    # Compiler identity for apples-to-apples regression comparison: the
    # baked-in `pli --version` string does NOT change across rebuilds, so
    # record the git hash plus the actual binary mtime. A differing id in
    # the previous run means deltas mix compiler changes with noise.
    try:
        rc, out, _ = run_command(["git", "rev-parse", "--short", "HEAD"],
                                 cwd=BENCHMARK_DIR.parent)
        git_hash = out.strip().split('\n')[0] if rc == 0 else "unknown"
    except Exception:
        git_hash = "unknown"
    try:
        pli_mtime = PLI.stat().st_mtime_ns
    except OSError:
        pli_mtime = 0
    info["compiler_id"] = f"{git_hash}:{pli_mtime}"
    return info


def pin_cmd(cmd: List[str]) -> List[str]:
    """Prepend process pinning flags for stable timings (P3).
    Linux: taskset -c 0 (pin to CPU 0). macOS: nice -n -20 (high priority).
    Windows: no argv wrapper exists, so the command is returned unchanged and
    run_command applies affinity/priority to the child handle instead.
    Single-runner is already guaranteed by sequential execution in the harness.
    Falls back to no pinning if the tool is unavailable (non-fatal)."""
    if IS_WINDOWS:
        return cmd
    if IS_MACOS:
        # best-effort nice; won't fail if we lack privileges
        return ["nice", "-n", "-20"] + cmd
    # Linux: taskset when present; otherwise run unpinned (a missing taskset
    # must not fail the harness — run_command also guards with OSError).
    if shutil.which("taskset") is None:
        return cmd
    return ["taskset", "-c", "0"] + cmd


def _pin_child_windows(proc: "subprocess.Popen") -> None:
    """Best-effort Windows equivalent of taskset+nice: pin the child to CPU 0
    and raise its priority class. Never raises; pinning is noise reduction,
    not correctness."""
    try:
        import ctypes
        kernel32 = ctypes.windll.kernel32
        # HIGH_PRIORITY_CLASS (0x80): above-normal without starving the OS the
        # way REALTIME would. Affinity mask 1 = CPU 0, mirroring taskset -c 0.
        kernel32.SetPriorityClass(proc._handle, 0x00000080)
        kernel32.SetProcessAffinityMask(proc._handle, 1)
    except Exception:
        pass


def run_command(cmd: List[str], cwd: Path = None, timeout: int = 300) -> Tuple[int, str, str]:
    """Run a command and return (returncode, stdout, stderr).

    encoding="utf-8" (not the locale default) so the ✓/≈/✗/⚠ output survives
    a C/POSIX locale on Linux even without PYTHONUTF8 set. On Windows the
    child is pinned to CPU 0 at high priority (see _pin_child_windows).
    """
    try:
        if IS_WINDOWS:
            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, cwd=cwd,
                                    text=True, encoding="utf-8")
            _pin_child_windows(proc)
            try:
                out, err = proc.communicate(timeout=timeout)
            except subprocess.TimeoutExpired:
                proc.kill()
                out, err = proc.communicate()
                return -1, out, "Timeout"
            return proc.returncode, out, err
        result = subprocess.run(cmd, capture_output=True, text=True,
                                encoding="utf-8", cwd=cwd, timeout=timeout)
        return result.returncode, result.stdout, result.stderr
    except subprocess.TimeoutExpired:
        return -1, "", "Timeout"
    except OSError as e:
        # Missing helper (e.g. taskset/nice) or spawn failure: report, don't crash.
        return -1, "", f"spawn failed: {e}"


# Stack size in bytes. Benchmarks with large stack-allocated arrays
# (array_matmul: 3x 400x400 ints = ~1.92MB; matrix_float: 3x 400x400 doubles
# = ~3.84MB) exceed the default 1MB Windows stack and crash with
# STATUS_STACK_OVERFLOW. 16MB gives headroom for all current benchmarks.
# This is a Windows-only link flag (/STACK is link.exe syntax and unknown to
# GNU ld); Linux/macOS default stacks (8MB) already fit, so no flag there.
STACK_SIZE = 16777216
IS_WINDOWS = os.name == "nt"
EXE_SUFFIX = ".exe" if IS_WINDOWS else ""


def _stack_flags() -> List[str]:
    if IS_WINDOWS:
        return ["-Wl,/STACK:" + str(STACK_SIZE)]
    return []


def _arch_flags() -> List[str]:
    # -march=x86-64 is only valid on x86_64 hosts; ARM (e.g. macOS arm64
    # runners) and other arches must not receive it.
    if platform.machine() in ("x86_64", "AMD64", "x64"):
        return ["-march=x86-64"]
    return []


def publish_effects(bench_name: str) -> Optional[Path]:
    """WS-A2 (ADR-267): publish the effects a benchmark's library unit promises.

    The library module is compiled on its own with --emit-effects, so the
    caller can consume the promise with --effects. Returns the file, or None
    when the benchmark has no library unit or the publish failed.
    """
    entry = PUBLISH_EFFECTS.get(bench_name)
    if not entry:
        return None
    src, out = entry
    eff = BIN_DIR / out
    cmd = [str(PLI), str(BENCHMARK_DIR / src), f"--emit-effects={eff}", "-c",
           "-o", str(BIN_DIR / f"{bench_name}_lib.o")]
    rc, sout, serr = run_command(cmd)
    if rc != 0:
        print(f"  effects publish failed [{bench_name}]:\n{sout}\n{serr}")
        return None
    return eff


def compile_pli(bench_name: str, pli_file, opt: str, output: Path,
                extra_flags: Optional[List[str]] = None) -> bool:
    """Compile a PL/I benchmark with pli at the given optimization level.

    *pli_file* is one source, or a list of sources for a program split across
    compilation units: the driver compiles and links them in one invocation.

    If *extra_flags* is None, the bench-level EXTRA_PLIFLAGS are used (P1.6).
    """
    srcs = [pli_file] if isinstance(pli_file, str) else list(pli_file)
    cmd = [str(PLI)] + [str(BENCHMARK_DIR / s) for s in srcs] + ["-o", str(output), opt]
    cmd.extend(_stack_flags())
    cmd.extend(extra_flags if extra_flags is not None
               else EXTRA_PLIFLAGS.get(bench_name, []))
    rc, out, err = run_command(cmd)
    if rc != 0:
        print(f"  PL/I compile failed [{bench_name} {opt}]:\n{out}\n{err}")
    return rc == 0


def compile_c(bench_name: str, c_file: str, opt: str, output: Path,
              compiler: Optional[str] = None) -> bool:
    """Compile a C benchmark with the given compiler at the given optimization level."""
    if compiler is None:
        compiler = CC
    src = BENCHMARK_DIR / c_file
    cmd = [compiler, opt] + _arch_flags() + [str(src), "-o", str(output)]
    cmd.extend(_stack_flags())
    if bench_name in BENCHMARKS_REQUIRING_MATH and not IS_WINDOWS:
        # -lm is POSIX-only; on Windows the math routines live in the CRT,
        # so no import library is needed (and no m.lib bootstrap either).
        cmd.append("-lm")
    rc, out, err = run_command(cmd)
    if rc != 0:
        print(f"  C compile failed [{bench_name} {opt}]:\n{out}\n{err}")
    return rc == 0


def run_benchmark(exe: Path, runs: int = RUNS_PER_BENCH,
                  warmup: int = 1) -> Tuple[List[float], str]:
    """Run a benchmark multiple times and return execution times in seconds.

    One warmup run is performed untimed (P1.2). Process pinning (P3) prepends
    taskset/nice for noise reduction. Returns (times, last_stdout).
    """
    times = []
    for _ in range(warmup):
        run_command(pin_cmd([str(exe)]), timeout=60)
    out = ""
    for _ in range(runs):
        start = time.perf_counter()
        rc, out, err = run_command(pin_cmd([str(exe)]), timeout=60)
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


def run_pair_comparisons(selected: Set[str], opt: str, runs: int) -> Dict:
    """Run the PAIRS table: one PL/I source compiled two ways.

    A pair prices a PL/I-specific cost that a C twin cannot show, by
    comparing two builds of the same program. The two builds must print the
    same output — a pair whose builds disagree is a bug, not a measurement.
    A pair may name different sources for its two sides, which is how the
    two-unit measurement compares a program split across compilation units
    with the same program in one unit.
    """
    results: Dict = {}
    if not PAIRS:
        return results
    src_of = {b[0]: b[1] for b in BENCHMARKS}
    src_of.update({b[0]: b[1] for b in MULTI_BENCHMARKS})
    tag = opt.replace("-", "")
    print(f"\n=== Paired PL/I-vs-PL/I comparisons ({opt}) ===")
    print(f"{'Benchmark':<16} {'A':<14} {'B':<14} "
          f"{'A (med)':>10} {'B (med)':>10} {'B/A':>8} {'Same':>6}")
    print("-" * 84)
    for entry in PAIRS:
        bench, label_a, flags_a, label_b, flags_b = entry[:5]
        src_b = entry[5] if len(entry) > 5 else None
        if bench not in src_of or bench not in selected:
            continue
        # The bench-level flags apply to both sides of the pair.
        # "@effects" stands for the flag that consumes the file the library
        # unit published.
        eff_file = publish_effects(bench)
        eff_flag = f"--effects={eff_file}" if eff_file else "@effects"
        extra_a = [(eff_flag if f == "@effects" else f) for f in flags_a] + \
            EXTRA_PLIFLAGS.get(bench, [])
        extra_b = [(eff_flag if f == "@effects" else f) for f in flags_b] + \
            EXTRA_PLIFLAGS.get(bench, [])
        exe_a = BIN_DIR / f"{bench}_pair_{label_a}_{tag}{EXE_SUFFIX}"
        exe_b = BIN_DIR / f"{bench}_pair_{label_b}_{tag}{EXE_SUFFIX}"
        if not compile_pli(bench, src_of[bench], opt, exe_a, extra_flags=extra_a):
            print(f"{bench:<16} {label_a:<14} PL/I COMPILE FAILED")
            results[f"{bench}:{label_a}"] = {"error": "pli_compile_failed"}
            continue
        if not compile_pli(bench, src_b if src_b else src_of[bench], opt, exe_b,
                           extra_flags=extra_b):
            print(f"{bench:<16} {label_b:<14} PL/I COMPILE FAILED")
            results[f"{bench}:{label_b}"] = {"error": "pli_compile_failed"}
            continue
        times_a, out_a = run_benchmark(exe_a, runs=runs)
        times_b, out_b = run_benchmark(exe_b, runs=runs)
        med_a = median_result(times_a)
        med_b = median_result(times_b)
        if not med_a or not med_b:
            print(f"{bench:<16} RUN FAILED")
            results[f"{bench}:{label_a}"] = {"error": "pli_run_failed"}
            continue
        same = "✓" if out_a.strip() == out_b.strip() else "✗"
        ratio = round(med_b / med_a, 4) if med_a > 0 else float("inf")
        key = f"{bench}:{label_a}->{label_b}"
        results[key] = {
            "benchmark": bench,
            "a": {"label": label_a, "flags": extra_a, "median": med_a},
            "b": {"label": label_b, "flags": extra_b, "median": med_b},
            "ratio": ratio,
            "same_output": same,
        }
        print(f"{bench:<16} {label_a:<14} {label_b:<14} "
              f"{med_a*1000:>10.2f} {med_b*1000:>10.2f} {ratio:>7.2f}x {same:>6}")
    return results


def needs_recompile(src: Path, exe: Path, force: bool = False) -> bool:
    """Skip recompilation if the executable is newer than the source (P1.5).

    *force* recompiles unconditionally: the skip only compares source and
    binary mtimes, so a compiler rebuild otherwise times stale binaries.
    """
    if force or not exe.exists():
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


def load_previous_results(current_filename: Optional[str] = None) -> Optional[Dict]:
    """Load the most recent previous benchmark results JSON.

    *current_filename* (the basename of the file about to be or just written)
    is excluded so that the current run never compares against itself (W7
    regression guard).  Returns the full payload (always normalized to
    {"machine": ..., "results": ...} so callers can compare compiler
    identity), or None if no usable previous run exists.
    """
    json_files = sorted(RESULTS_DIR.glob("benchmark_results_*.json"),
                        key=lambda f: f.stat().st_mtime, reverse=True)
    if not json_files:
        return None
    for f in json_files:
        if current_filename and f.name == current_filename:
            continue
        try:
            with open(f, encoding="utf-8") as fh:
                data = json.load(fh)
            # Support both old (flat results) and new ({"machine", "results"}) formats
            if isinstance(data, dict) and "results" in data:
                return data
            return {"machine": {}, "results": data}
        except (json.JSONDecodeError, OSError):
            continue
    return None


def check_for_deterioration(results: Dict, previous: Dict) -> List[str]:
    """Compare current -O2 ratios against the previous run.

    Returns a list of warning strings for benchmarks whose ratio worsened
    (increased) by more than DETERIORATION_THRESHOLD (5 % relative).
    """
    warnings = []
    for bench_name, _, _ in BENCHMARKS:
        cur = results.get(bench_name, {}).get("-O2", {})
        prev = previous.get(bench_name, {}).get("-O2", {})
        cur_ratio = cur.get("ratio")
        prev_ratio = prev.get("ratio")
        if cur_ratio is None or prev_ratio is None:
            continue
        if prev_ratio <= 0:
            continue
        rel_change = (cur_ratio - prev_ratio) / prev_ratio
        if rel_change > DETERIORATION_THRESHOLD:
            warnings.append(
                f"  {bench_name:<20} ⚠ ratio {cur_ratio:.2f}x vs prev "
                f"{prev_ratio:.2f}x (+{rel_change*100:.1f}%)"
            )
    return warnings


def main():
    import argparse
    # The report prints UTF-8 marks (the check mark, the em dash). A Windows
    # console defaults to the ANSI code page, where they are unencodable, and
    # the print raises UnicodeEncodeError: write UTF-8 and replace whatever the
    # console cannot take.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    parser = argparse.ArgumentParser(
        description="PL/I compiler benchmark harness")
    parser.add_argument("--checks-forced", action="store_true",
                        help="compile every benchmark with --size-checks "
                             "--subscript-checks at -O2 and require "
                             "identical stdout to the default run (P1.7)")
    parser.add_argument("--runs", type=int, default=RUNS_PER_BENCH,
                        help="timed runs per benchmark (default: %(default)s)")
    parser.add_argument("--cc", type=str, default=None,
                        help="C compiler to use (overrides CC env var, "
                             "default: clang on Unix/macOS, clang.exe on Windows)")
    parser.add_argument("--pli", type=str, default=None,
                        help="pli binary to use (overrides PLI env var)")
    parser.add_argument("--force", action="store_true",
                        help="recompile every benchmark even if the binary "
                             "is newer than the source (use after a compiler "
                             "rebuild, otherwise stale binaries are timed)")
    parser.add_argument("--only", type=str, default=None, metavar="BENCHES",
                        help="comma-separated subset of benchmarks to run "
                             "(e.g. --only proc_calls,string_ops); "
                             "default: all 28 (21 in ir/, 7 in lang/)")
    args = parser.parse_args()

    # Use --cc argument if provided, otherwise fall back to env var, then default
    cc_cmd = args.cc or os.environ.get("CC", _default_cc())
    pli_cmd = args.pli or os.environ.get("PLI", str(_find_pli()))
    global PLI
    PLI = Path(pli_cmd)

    print("PL/I Compiler Benchmark Harness")
    print("=" * 70)

    if not PLI.exists():
        print(f"Error: pli not found at {PLI}")
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

    # --only subset (the plan docs invoke --only throughout; running one
    # witness at a time keeps the 5-run ritual fast and the noise down).
    benches = BENCHMARKS
    multi_names = {b[0] for b in MULTI_BENCHMARKS}
    if args.only:
        wanted = [b.strip() for b in args.only.split(",") if b.strip()]
        known = {b[0] for b in BENCHMARKS} | multi_names
        unknown = [b for b in wanted if b not in known]
        if unknown:
            print(f"Error: unknown benchmark(s): {', '.join(unknown)}")
            print(f"Known: {', '.join(sorted(known))}")
            return 2
        benches = [b for b in BENCHMARKS if b[0] in wanted]
        pair_names = {b[0] for b in benches} | (multi_names & set(wanted))
        picked = [b[0] for b in benches] + sorted(multi_names & set(wanted))
        print(f"Running subset: {', '.join(picked)}")
    else:
        pair_names = {b[0] for b in BENCHMARKS} | multi_names

    for bench_name, pli_file, c_file in benches:
        results[bench_name] = {}
        src_pli = BENCHMARK_DIR / pli_file
        src_c = BENCHMARK_DIR / c_file

        default_flags = EXTRA_PLIFLAGS.get(bench_name, [])
        forced_flags = CHECKS_FORCED_FLAGS

        for opt in opt_levels:
            tag = opt.replace('-', '')
            pli_exe = BIN_DIR / f"{bench_name}_pli_{tag}{EXE_SUFFIX}"
            c_exe = BIN_DIR / f"{bench_name}_c_{tag}{EXE_SUFFIX}"

            if args.checks_forced:
                pli_exe_forced = BIN_DIR / f"{bench_name}_pli_forced_{tag}{EXE_SUFFIX}"
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
                if not compile_c(bench_name, c_file, opt, c_exe,
                                 compiler=cc_cmd):
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
            if needs_recompile(src_pli, pli_exe, args.force):
                if not compile_pli(bench_name, pli_file, opt, pli_exe):
                    print(f"{bench_name:<20} {opt:<6} PL/I COMPILE FAILED")
                    results[bench_name][opt] = {"error": "pli_compile_failed"}
                    continue
            if needs_recompile(src_c, c_exe, args.force):
                if not compile_c(bench_name, c_file, opt, c_exe,
                                 compiler=cc_cmd):
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
            c_min = min(c_times)
            ratio = pli_median / c_median if c_median > 0 else float('inf')
            # Min-of-runs ratio: the investigation ritual (P0) compares best
            # runs, which is what compiler changes move; the median is robust
            # but load-sensitive. Record both, gate on the median.
            min_ratio = pli_min / c_min if c_min > 0 else float('inf')

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
                "min_ratio": round(min_ratio, 4),
                "match": out_match,
            }
            print(f"{bench_name:<20} {opt:<6} {flags_str:<24} "
                  f"{pli_min*1000:>10.2f} {pli_median*1000:>10.2f} "
                  f"{c_median*1000:>10.2f} {ratio:>7.2f}x {out_match:>6}")

    # The paired PL/I-vs-PL-I comparisons: one source compiled two ways.
    # They are a -O2 measurement (the gate the optimization plan sets), so
    # they run once, not at every optimization level.
    pairs = run_pair_comparisons(pair_names, "-O2", args.runs)

    # Save JSON with machine metadata (P3)
    json_file = RESULTS_DIR / f"benchmark_results_{ts}.json"
    machine_info = get_machine_info()
    with open(json_file, "w", encoding="utf-8") as f:
        json.dump({"machine": machine_info, "results": results,
                   "pairs": pairs}, f, indent=2)

    # Save CSV (P1.5)
    csv_file = RESULTS_DIR / f"summary_{ts}.csv"
    with open(csv_file, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["benchmark", "opt", "flags", "pli_min_ms",
                    "pli_median_ms", "c_median_ms", "ratio", "match"])
        for bench_name, _, _ in benches:
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
        for bench_name, _, _ in benches:
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
    for bench_name, _, _ in benches:
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

    # Average ratio by optimization level, per category. The ir corpus's
    # geomean is the project's acceptance gate, so the lang corpus — whose
    # benchmarks price PL/I-only costs — is reported on its own and never
    # dilutes or inflates that gate.
    ir_names = {b[0] for b in benches if b[1].startswith("ir/")}
    lang_names = {b[0] for b in benches if b[1].startswith("lang/")}
    for label, names in (("ir (C parity — the acceptance gate)", ir_names),
                          ("lang (PL/I-specific)", lang_names)):
        if not names:
            continue
        print(f"\nAverage PL/I vs C ratio by optimization level [{label}]:")
        for opt in OPT_LEVELS:
            ratios = []
            for bench_name in names:
                data = results.get(bench_name, {}).get(opt, {})
                if "ratio" in data:
                    ratios.append(data["ratio"])
            if ratios:
                avg = statistics.mean(ratios)
                # The acceptance gate is a geomean (<0.90x) — print it, not just
                # the arithmetic mean, which one outlier can drag.
                finite = [r for r in ratios if math.isfinite(r) and r > 0]
                geo = math.exp(sum(math.log(r) for r in finite) / len(finite)) if finite else float('nan')
                print(f"  {opt}: avg ratio = {avg:.2f}x geomean = {geo:.2f}x "
                      f"(min={min(ratios):.2f}x, max={max(ratios):.2f}x)")

    # Timing + parity validation (W7)
    print("\n=== Timing & Parity Checks (W7) ===")
    all_checks_ok = True
    for bench_name, _, _ in benches:
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
        # Timing note (not a gate): below the noise floor the ratio is
        # indicative only. Fast benches now live here *by success*, so this
        # must never fail the run — only stdout mismatch does that.
        if pli_med < min_ms:
            print(f"  {bench_name:<20} ◦ {pli_med:.1f}ms < {min_ms}ms noise floor", end="")
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
            # decimal_ops uses exact decimal arithmetic; its C baseline uses
            # double — expected ~ mismatch (see DECIMAL-PERF-PLAN.md §8 non-goal).
            # bcd_decimal is NOT here: WS-E3 (ADR-248) gave it an exact
            # __int128 baseline, so its output must match exactly.
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

    # Regression guard: compare against the most recent previous run (W7)
    prev_full = load_previous_results(json_file.name)
    previous = prev_full.get("results") if isinstance(prev_full, dict) else None
    if previous is not None:
        prev_id = prev_full.get("machine", {}).get("compiler_id")
        cur_id = machine_info.get("compiler_id")
        if prev_id and cur_id and prev_id != cur_id:
            print("\n=== COMPILER CHANGE NOTICE ===")
            print(f"  Previous run used compiler {prev_id}; this run uses {cur_id}.")
            print("  Ratio deltas below mix compiler changes with noise — "
                  "re-baseline before trusting them.")
        warnings = check_for_deterioration(results, previous)
        if warnings:
            print("\n=== REGRESSION WARNINGS ===")
            for w in warnings:
                print(w)
            print(f"\n⚠ {len(warnings)} benchmark(s) deteriorated by >"
                  f"{int(DETERIORATION_THRESHOLD*100)}% at -O2 vs previous run")
        else:
            print("\n=== REGRESSION CHECK ===")
            print("  ✓ No material deterioration detected at -O2 vs previous run")
    else:
        print("\n=== REGRESSION CHECK ===")
        print("  — No previous results found to compare against")

    return 0 if all_checks_ok else 1


if __name__ == "__main__":
    sys.exit(main())
