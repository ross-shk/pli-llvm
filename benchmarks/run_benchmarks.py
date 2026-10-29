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
import uuid
from pathlib import Path
from typing import Dict, List, Set, Tuple, Optional

from bench_common import MeasureError, probe_pinning, run_measured, sha256_file

BENCHMARK_DIR = Path(__file__).parent

# Result files carry a schema version and a manifest of everything that could
# change a ratio, so a regression check never silently compares two different
# measurements (P1 reproducibility).
SCHEMA_VERSION = 2


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
    # LLM orchestration (benchmarks/LLM-ORCHESTRATION-PLAN.md): the host-side
    # CPU work around an inference engine. Each has an exact C twin, so the
    # ratio prices the orchestration loop itself, not the model math.
    ("llm_tokenize",      "ir/llm_tokenize.pli",      "ir/baseline/llm_tokenize_c.c"),
    ("llm_sample",        "ir/llm_sample.pli",        "ir/baseline/llm_sample_c.c"),
    ("llm_kvcache",       "ir/llm_kvcache.pli",       "ir/baseline/llm_kvcache_c.c"),
    ("llm_grammar",       "ir/llm_grammar.pli",       "ir/baseline/llm_grammar_c.c"),
    ("llm_expert_tier",   "ir/llm_expert_tier.pli",   "ir/baseline/llm_expert_tier_c.c"),
    ("llm_lookup",        "ir/llm_lookup.pli",        "ir/baseline/llm_lookup_c.c"),
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


def _find_builtins_lib(compiler: str) -> Optional[str]:
    """Path to clang_rt.builtins on Windows, where clang.exe does not auto-link it."""
    if not IS_WINDOWS:
        return None
    rc, out, _ = run_command([compiler, "-print-runtime-dir"])
    if rc == 0 and out.strip():
        lib = Path(out.strip()) / "clang_rt.builtins-x86_64.lib"
        if lib.is_file():
            return str(lib)
    return None


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
# The C twin follows the PL/I target policy by default; --c-native
# builds it for the host CPU instead (see _c_policy_flags).
C_NATIVE = False
# --c-musl builds the C twin against the same static musl libc as the Linux
# PL/I output, so a ratio measures codegen rather than glibc-vs-musl.
C_STATIC_MUSL = False
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
# One untimed warmup per side: pays for page-in and first-run effects, and
# fixes the expected output every timed run must reproduce (P1.2).
WARMUP = 1

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
    # LLM orchestration: integer-only, exact parity with the C twin.
    "llm_tokenize":     {"min_ms_O2": 50, "float_tol": False},
    "llm_sample":       {"min_ms_O2": 50, "float_tol": False},
    "llm_kvcache":      {"min_ms_O2": 50, "float_tol": False},
    "llm_grammar":      {"min_ms_O2": 50, "float_tol": False},
    "llm_expert_tier":  {"min_ms_O2": 50, "float_tol": False},
    "llm_lookup":       {"min_ms_O2": 50, "float_tol": False},
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
    "pure_call":        {"min_ms_O2": 50, "float_tol": False},
    "bit_merge":        {"min_ms_O2": 50, "float_tol": False},
    "by_name":          {"min_ms_O2": 50, "float_tol": False},
    "select_chain":     {"min_ms_O2": 50, "float_tol": False},
}

# Benchmarks whose C twin is a *descriptive* reference, not a parity twin: the
# two programs compute with different number systems, so their output can never
# match and their ratio prices PL/I's decimal work against C doubles. They are
# reported, but excluded from the C-parity acceptance gate (ADR-262): a gate row
# that is by design unequal would either fail forever or be waived forever.
NON_PARITY = {
    "decimal_ops": "PL/I fixed decimal vs C double (DECIMAL-PERF-PLAN.md §8)",
}

# The rows the -O2 acceptance gate requires: every IR benchmark except the
# descriptive-only ones. A missing, failed or mismatched row voids the gate.
GATE_BENCHMARKS = [b[0] for b in IR_BENCHMARKS if b[0] not in NON_PARITY]


def get_machine_info(cc_cmd: str = None) -> Dict:
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
        rc, out, _ = run_command([cc_cmd or CC, "--version"])
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


# Process pinning (P3): probed once, and recorded in the manifest. The probe
# exists because a wrapper can be present and still refuse: macOS `nice`
# exits 0 after "setpriority: Permission denied".
PINNING: Dict = {"mode": "unprobed", "prefix": [], "applied": False}


def resolve_pinning() -> Dict:
    """Probe and cache the pinning prefix for this host."""
    global PINNING
    PINNING = probe_pinning()
    return PINNING


def run_command(cmd: List[str], cwd: Path = None, timeout: int = 300) -> Tuple[int, str, str]:
    """Run a command and return (returncode, stdout, stderr).

    Used for compiler invocations and metadata probes, never for timed runs
    (those go through bench_common.run_measured, which validates the run).
    encoding="utf-8" (not the locale default) so the ✓/≈/✗/⚠ output survives
    a C/POSIX locale on Linux even without PYTHONUTF8 set.
    """
    try:
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


def _c_policy_flags() -> List[str]:
    """Flags that make the C twin match the PL/I build's target policy.

    The C twin must be built for the same CPU and floating-point policy as
    the PL/I output. The driver targets LLVM's generic CPU (src/target.cpp)
    and does not contract FP operations, so C gets the generic CPU and
    -ffp-contract=off. Without this the C baseline would get the host's
    native tuning (e.g. apple-m1) and fused multiply-adds, and the ratio
    would price the target policy rather than code generation.
    """
    machine = platform.machine().lower()
    flags: List[str] = []
    if not C_NATIVE:
        if machine in ("x86_64", "amd64", "x64"):
            flags.append("-march=x86-64")
        elif machine in ("arm64", "aarch64"):
            flags.append("-mcpu=generic")
    # PL/I does not contract FP operations, so neither does its C twin.
    flags.append("-ffp-contract=off")
    return flags


def _musl_target() -> str:
    """The static-musl target triple matching the host architecture."""
    machine = platform.machine().lower()
    arch = "aarch64" if machine in ("arm64", "aarch64") else "x86_64"
    return f"{arch}-linux-musl"


def _c_runtime_flags() -> List[str]:
    """Flags that make the C twin use the PL/I output's static musl libc."""
    if not C_STATIC_MUSL:
        return []
    return [f"--target={_musl_target()}", "-static"]


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
    cmd = ([compiler, opt] + _c_policy_flags() + _c_runtime_flags()
           + [str(src), "-o", str(output)])
    cmd.extend(_stack_flags())
    if bench_name in BENCHMARKS_REQUIRING_MATH and not IS_WINDOWS:
        # -lm is POSIX-only; on Windows the math routines live in the CRT,
        # so no import library is needed (and no m.lib bootstrap either).
        cmd.append("-lm")
    _builtins = _find_builtins_lib(compiler)
    if _builtins:
        cmd.append(_builtins)
    rc, out, err = run_command(cmd)
    if rc != 0:
        print(f"  C compile failed [{bench_name} {opt}]:\n{out}\n{err}")
    return rc == 0


def run_benchmark(exe: Path, runs: int = RUNS_PER_BENCH, warmup: int = WARMUP,
                  cwd: Optional[Path] = None) -> Tuple[str, List[float], str]:
    """Run *exe* and return (status, times_seconds, stdout).

    One warmup run is performed untimed (P1.2) and fixes the expected output.
    Every later run must exit 0 and print that same output: a crashed run, a
    timeout or an unstable workload invalidates the measurement instead of
    silently contributing a timing (or an unrelated checksum) to the median.
    """
    expected = None
    try:
        for _ in range(warmup):
            _, out = run_measured([str(exe)], cwd=cwd, prefix=PINNING["prefix"])
            expected = out.strip()
    except MeasureError as exc:
        print(f"  Warmup failed: {exc}")
        return "warmup_failed", [], ""
    times = []
    for _ in range(runs):
        try:
            secs, _ = run_measured([str(exe)], cwd=cwd, expected_stdout=expected,
                                   prefix=PINNING["prefix"])
        except MeasureError as exc:
            print(f"  Run failed: {exc}")
            return exc.status, [], ""
        times.append(secs)
    return "ok", times, expected or ""


def run_pair_benchmark(pli_exe: Path, c_exe: Path, runs: int = RUNS_PER_BENCH,
                       warmup: int = WARMUP, cwd: Optional[Path] = None
                       ) -> Tuple[str, List[float], str, str, List[float], str]:
    """Run PL/I and C alternately and return both sides' samples.

    Alternating the two processes keeps a drifting CPU clock from pricing one
    side systematically higher: a slow patch is shared by both instead of
    landing entirely on whichever side happened to run second.
    """
    expected_pli = expected_c = None
    try:
        for _ in range(warmup):
            _, out_pli = run_measured([str(pli_exe)], cwd=cwd,
                                      prefix=PINNING["prefix"])
            _, out_c = run_measured([str(c_exe)], cwd=cwd,
                                    prefix=PINNING["prefix"])
            expected_pli = out_pli.strip()
            expected_c = out_c.strip()
    except MeasureError as exc:
        print(f"  Warmup failed: {exc}")
        return "warmup_failed", [], "", "warmup_failed", [], ""

    pli_times: List[float] = []
    c_times: List[float] = []
    for _ in range(runs):
        try:
            secs, _ = run_measured([str(pli_exe)], cwd=cwd,
                                   expected_stdout=expected_pli,
                                   prefix=PINNING["prefix"])
        except MeasureError as exc:
            print(f"  PL/I run failed: {exc}")
            return exc.status, [], "", "skipped", [], ""
        pli_times.append(secs)
        try:
            secs, _ = run_measured([str(c_exe)], cwd=cwd,
                                   expected_stdout=expected_c,
                                   prefix=PINNING["prefix"])
        except MeasureError as exc:
            print(f"  C run failed: {exc}")
            return "ok", pli_times, expected_pli or "", exc.status, [], ""
        c_times.append(secs)
    return "ok", pli_times, expected_pli or "", "ok", c_times, expected_c or ""


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
        # One stable key per comparison, whatever happens to it.
        pair_id = f"{bench}:{label_a}->{label_b}"
        record = {"benchmark": bench, "status": "ok", "a": {"label": label_a},
                  "b": {"label": label_b}}

        # Publication is built only when a side consumes it: a pair without
        # "@effects" must not depend on an unrelated publish succeeding, and a
        # failed publish must never reach the compiler as a literal flag.
        eff_flag = None
        if "@effects" in flags_a or "@effects" in flags_b:
            eff_file = publish_effects(bench)
            if eff_file is None:
                record["status"] = "effects_publish_failed"
                results[pair_id] = record
                print(f"{bench:<16} {pair_id} EFFECTS PUBLISH FAILED")
                continue
            eff_flag = f"--effects={eff_file}"

        def side_flags(flags: List[str]) -> List[str]:
            """Resolve @effects and add the benchmark's required flags."""
            resolved = [eff_flag if f == "@effects" else f for f in flags]
            return resolved + EXTRA_PLIFLAGS.get(bench, [])

        extra_a = side_flags(flags_a)
        extra_b = side_flags(flags_b)
        exe_a = BIN_DIR / f"{bench}_pair_{label_a}_{tag}{EXE_SUFFIX}"
        exe_b = BIN_DIR / f"{bench}_pair_{label_b}_{tag}{EXE_SUFFIX}"
        if not compile_pli(bench, src_of[bench], opt, exe_a, extra_flags=extra_a):
            record["status"] = "a_compile_failed"
            results[pair_id] = record
            print(f"{bench:<16} {label_a:<14} PL/I COMPILE FAILED")
            continue
        if not compile_pli(bench, src_b if src_b else src_of[bench], opt, exe_b,
                           extra_flags=extra_b):
            record["status"] = "b_compile_failed"
            results[pair_id] = record
            print(f"{bench:<16} {label_b:<14} PL/I COMPILE FAILED")
            continue
        status_a, times_a, out_a = run_benchmark(exe_a, runs=runs, cwd=BIN_DIR)
        status_b, times_b, out_b = run_benchmark(exe_b, runs=runs, cwd=BIN_DIR)
        if status_a != "ok" or status_b != "ok":
            record["status"] = f"a_{status_a}" if status_a != "ok" else f"b_{status_b}"
            results[pair_id] = record
            print(f"{bench:<16} {pair_id} RUN FAILED ({record['status']})")
            continue
        med_a = median_result(times_a)
        med_b = median_result(times_b)
        same = "✓" if out_a.strip() == out_b.strip() else "✗"
        ratio = round(med_b / med_a, 4) if med_a > 0 else float("inf")
        record["a"].update({"flags": extra_a, "median": med_a, "runs": times_a,
                            "output": out_a.strip()})
        record["b"].update({"flags": extra_b, "median": med_b, "runs": times_b,
                            "output": out_b.strip()})
        record["ratio"] = ratio
        record["same_output"] = same
        # A pair whose builds disagree is a bug, not a measurement.
        if same != "✓":
            record["status"] = "output_mismatch"
        results[pair_id] = record
        print(f"{bench:<16} {label_a:<14} {label_b:<14} "
              f"{med_a*1000:>10.2f} {med_b*1000:>10.2f} {ratio:>7.2f}x {same:>6}")
    return results


def outputs_agree(bench_name: str, pli_out: str, c_out: str) -> str:
    """The parity mark for a benchmark row.

    A NON_PARITY benchmark is descriptive (`~`): its C twin is a different
    algorithm, so the row is reported but never gates. Everything else must
    match exactly, or tolerantly when it prints floating point.
    """
    if bench_name in NON_PARITY:
        return "~"
    return outputs_match(pli_out, c_out, bench_name)


def row_is_valid(row: Dict) -> bool:
    """Whether a result row may be published as a ratio or feed the gate."""
    if row.get("status") != "ok":
        return False
    if row.get("match") not in ("✓", "≈", "~"):
        return False
    ratio = row.get("ratio")
    return isinstance(ratio, (int, float)) and math.isfinite(ratio) and ratio > 0


def accepted_geomean(results: Dict, expected: List[str],
                     opt: str = "-O2") -> Optional[float]:
    """The corpus geomean, or None unless every expected row is valid.

    A partial run, a failed row or a mismatched output voids the gate rather
    than quietly shrinking the set of rows it averages over (ADR-262).
    """
    ratios = []
    for name in expected:
        row = results.get(name, {}).get(opt, {})
        if not row_is_valid(row) or row.get("match") == "~":
            return None
        ratios.append(row["ratio"])
    if not ratios:
        return None
    return math.exp(statistics.mean(math.log(r) for r in ratios))


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


def load_previous_results(current_filename: Optional[str] = None,
                          explicit: Optional[str] = None) -> Optional[Dict]:
    """Load the results a regression check compares against.

    *explicit* names a file (--compare-to). Otherwise the newest saved run that
    is not *current_filename* is used, so a run never compares against itself.
    Old-format payloads are still readable but carry no manifest, which makes
    them non-comparable rather than silently comparable.
    """
    if explicit:
        try:
            with open(explicit, encoding="utf-8") as fh:
                data = json.load(fh)
        except (json.JSONDecodeError, OSError) as exc:
            print(f"  cannot read --compare-to {explicit}: {exc}")
            return None
        return normalize_payload(data)
    json_files = sorted(RESULTS_DIR.glob("benchmark_results_*.json"),
                        key=lambda f: f.stat().st_mtime, reverse=True)
    for f in json_files:
        if current_filename and f.name == current_filename:
            continue
        try:
            with open(f, encoding="utf-8") as fh:
                data = json.load(fh)
        except (json.JSONDecodeError, OSError):
            continue
        return normalize_payload(data)
    return None


def normalize_payload(data) -> Dict:
    """Accept both the old flat results map and the current {"results": ...}."""
    if isinstance(data, dict) and "results" in data:
        return data
    return {"machine": {}, "manifest": {}, "results": data}


def manifest_mismatch(current: Dict, previous: Dict) -> Optional[str]:
    """Why two runs are not comparable, or None when their settings match.

    Compiler identity is deliberately not part of this: detecting what a new
    compiler did to a ratio is the point of the guard. Settings, workload set,
    flags and pinning are, because a delta across them measures the setup.
    """
    prev = previous.get("manifest") or {}
    if not prev:
        return "previous run has no manifest (pre-v2 results)"
    for key in ("benchmarks", "opt_levels", "runs", "warmup",
                "checks_forced", "extra_pli_flags", "pli_flags",
                "c_runtime", "c_flags", "sampling", "platform"):
        if prev.get(key) != current.get(key):
            return f"manifest differs on {key}"
    return None


def check_for_deterioration(results: Dict, previous: Dict,
                            names: List[str]) -> Tuple[List[str], int]:
    """Compare current -O2 ratios against a compatible previous run.

    Returns (warnings, compared_rows). Only rows valid on both sides count, so
    "no deterioration" always says how many rows it actually looked at.
    """
    warnings = []
    compared = 0
    for bench_name in names:
        cur = results.get(bench_name, {}).get("-O2", {})
        prev = previous.get(bench_name, {}).get("-O2", {})
        if not row_is_valid(cur) or not row_is_valid(prev):
            continue
        cur_ratio = cur["ratio"]
        prev_ratio = prev["ratio"]
        compared += 1
        rel_change = (cur_ratio - prev_ratio) / prev_ratio
        if rel_change > DETERIORATION_THRESHOLD:
            # The PL/I spread says whether the delta is bigger than the noise
            # the threshold is meant to ignore.
            pli = cur.get("pli", {})
            spread = (pli.get("stdev", 0.0) / pli["median"] * 100
                      if pli.get("median") else 0.0)
            warnings.append(
                f"  {bench_name:<20} ⚠ ratio {cur_ratio:.2f}x vs prev "
                f"{prev_ratio:.2f}x (+{rel_change*100:.1f}%, "
                f"PL/I spread ±{spread:.0f}%)"
            )
    return warnings, compared


def build_manifest(pli: Path, cc_cmd: str, benches: List[Tuple],
                   opt_levels: List[str], runs: int, warmup: int,
                   checks_forced: bool) -> Dict:
    """Everything that can change a ratio, so comparability is checkable.

    Source contents are hashed rather than mtime-compared: two files can share
    a timestamp, and a checkout can rewrite one without changing it.
    """
    sources = {}
    for name, pli_file, c_file in benches:
        for rel in (pli_file, c_file):
            sources[rel] = sha256_file(BENCHMARK_DIR / rel)
    return {
        "schema": SCHEMA_VERSION,
        "benchmarks": sorted(b[0] for b in benches),
        "opt_levels": opt_levels,
        "runs": runs,
        "warmup": warmup,
        "sampling": "interleaved",
        "checks_forced": checks_forced,
        "extra_pli_flags": [" ".join(EXTRA_PLIFLAGS.get(b[0], []))
                            for b in benches],
        "pli_binary": str(pli),
        "pli_sha256": sha256_file(pli),
        "cc": cc_cmd,
        "c_runtime": "musl-static" if C_STATIC_MUSL else "host",
        "c_flags": _c_policy_flags() + _c_runtime_flags(),
        "pinning": PINNING,
        "platform": platform.platform(aliased=True),
        "sources": sources,
    }


def write_atomic(path: Path, write) -> None:
    """Write through a temporary file: an interrupted run leaves no half-file."""
    tmp = path.with_suffix(path.suffix + ".tmp")
    with open(tmp, "w", newline="", encoding="utf-8") as fh:
        write(fh)
    os.replace(tmp, path)


def validate_inventory() -> List[str]:
    """Check that the registration tables agree with the source tree.

    Registration is the single list the runner trusts; a source that is
    missing, a duplicate name or a gate row without timing metadata is an
    inventory bug that should stop the run before it builds anything.
    """
    problems = []
    seen = set()
    for name, pli_file, c_file in BENCHMARKS:
        if name in seen:
            problems.append(f"duplicate benchmark name: {name}")
        seen.add(name)
        for rel in (pli_file, c_file):
            if not (BENCHMARK_DIR / rel).is_file():
                problems.append(f"{name}: missing source {rel}")
        if name not in CHECKS:
            problems.append(f"{name}: no CHECKS entry (timing metadata)")
    for name, sources in MULTI_BENCHMARKS:
        for rel in sources:
            if not (BENCHMARK_DIR / rel).is_file():
                problems.append(f"{name}: missing source {rel}")
    known = seen | {b[0] for b in MULTI_BENCHMARKS}
    for entry in PAIRS:
        if entry[0] not in known:
            problems.append(f"pair {entry[0]}: benchmark not registered")
        for rel in (entry[5] if len(entry) > 5 else []):
            if not (BENCHMARK_DIR / rel).is_file():
                problems.append(f"pair {entry[0]}: missing side-B source {rel}")
    for name in NON_PARITY:
        if name not in seen:
            problems.append(f"NON_PARITY names unknown benchmark {name}")
    return problems


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
    parser.add_argument("--c-native", action="store_true",
                        help="build the C twin for the host CPU instead of the "
                             "PL/I target policy (default: matched generic CPU)")
    parser.add_argument("--c-musl", action="store_true",
                        help="Linux only: build the C twin with the same static "
                             "musl libc as the PL/I output (requires clang and "
                             "the musl target libraries)")
    parser.add_argument("--cc", type=str, default=None,
                        help="C compiler to use (overrides CC env var, "
                             "default: clang on Unix/macOS, clang.exe on Windows)")
    parser.add_argument("--pli", type=str, default=None,
                        help="pli binary to use (overrides PLI env var)")
    parser.add_argument("--force", action="store_true",
                        help="accepted for compatibility; every measured binary "
                             "is now always recompiled")
    parser.add_argument("--compare-to", type=str, default=None, metavar="JSON",
                        help="baseline results file for the regression check "
                             "(default: the newest compatible saved run)")
    parser.add_argument("--only", type=str, default=None, metavar="BENCHES",
                        help="comma-separated subset of benchmarks to run "
                             "(e.g. --only proc_calls,string_ops); "
                             f"default: all {len(BENCHMARKS)} "
                             f"({len(IR_BENCHMARKS)} in ir/, "
                             f"{len(LANG_BENCHMARKS)} in lang/)")
    args = parser.parse_args()
    global C_NATIVE, C_STATIC_MUSL
    C_NATIVE = args.c_native
    C_STATIC_MUSL = args.c_musl

    # Reject nonsensical sample counts before touching the filesystem: a run
    # with zero timed samples has no median to report.
    if args.runs < 1:
        parser.error("--runs must be at least 1")
    if C_STATIC_MUSL:
        if platform.system() != "Linux":
            parser.error("--c-musl is Linux-only")
        libc = Path(f"/usr/lib/{_musl_target()}/libc.a")
        if not libc.is_file():
            parser.error(f"--c-musl needs {libc} (install the musl target "
                         f"libraries, e.g. musl-tools)")

    # Use --cc argument if provided, otherwise fall back to env var, then default
    cc_cmd = args.cc or os.environ.get("CC", _default_cc())
    if C_STATIC_MUSL and "clang" not in Path(cc_cmd).name.lower():
        parser.error("--c-musl requires clang (pass --cc clang)")
    pli_cmd = args.pli or os.environ.get("PLI", str(_find_pli()))
    global PLI
    PLI = Path(pli_cmd)

    print("PL/I Compiler Benchmark Harness")
    print("=" * 70)
    if C_STATIC_MUSL:
        print(f"C twin runtime: static {_musl_target()} (matches the PL/I libc)")
    if args.force:
        print("Note: --force is redundant; every measured binary is rebuilt.")

    if not PLI.exists():
        print(f"Error: pli not found at {PLI}")
        print("Build first: cmake -G Ninja -S . -B build/cmake && "
              "cmake --build build/cmake -j$(nproc)")
        return 1

    opt_levels = ["-O2"] if args.checks_forced else OPT_LEVELS

    # Inventory sanity before any build: a registered source that is missing or
    # a row without timing metadata would otherwise surface mid-run.
    problems = validate_inventory()
    if problems:
        print("Error: benchmark inventory is inconsistent:")
        for problem in problems:
            print(f"  {problem}")
        return 2

    # Pinning is probed once: a wrapper that is present but refuses (macOS
    # `nice` without the privilege) must be reported, not assumed.
    resolve_pinning()
    print(f"Pinning: {PINNING['mode']} — {PINNING['note']}")

    # Header for the comparison table
    print(f"\n{'Benchmark':<20} {'Opt':<6} {'Flags':<24} {'PL/I (min)':>10} "
          f"{'PL/I (med)':>10} {'C (med)':>10} {'Ratio':>8} {'Match':>6}")
    print("-" * 84)

    results: Dict = {}
    ts = int(time.time())
    run_id = f"{ts}_{uuid.uuid4().hex[:8]}"

    # --only subset (the plan docs invoke --only throughout; running one
    # witness at a time keeps the 5-run ritual fast and the noise down).
    benches = BENCHMARKS
    multi_names = {b[0] for b in MULTI_BENCHMARKS}
    if args.only:
        wanted = [b.strip() for b in args.only.split(",") if b.strip()]
        if not wanted:
            print("Error: --only needs at least one benchmark name")
            return 2
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

        default_flags = EXTRA_PLIFLAGS.get(bench_name, [])

        for opt in opt_levels:
            tag = opt.replace('-', '')
            pli_exe = BIN_DIR / f"{bench_name}_pli_{tag}{EXE_SUFFIX}"
            c_exe = BIN_DIR / f"{bench_name}_c_{tag}{EXE_SUFFIX}"

            if args.checks_forced:
                pli_exe_forced = BIN_DIR / f"{bench_name}_pli_forced_{tag}{EXE_SUFFIX}"
                # Forced checks are additive: the benchmark's own required
                # flags stay on, otherwise the parity check compares two
                # different programs.
                forced_flags = list(dict.fromkeys(default_flags + CHECKS_FORCED_FLAGS))
                forced_flags_str = " ".join(forced_flags)

                if not compile_pli(bench_name, pli_file, opt, pli_exe):
                    print(f"{bench_name:<20} {opt:<6} PL/I COMPILE FAILED")
                    results[bench_name][opt] = {"status": "pli_compile_failed"}
                    continue
                if not compile_pli(bench_name, pli_file, opt, pli_exe_forced,
                                   extra_flags=forced_flags):
                    print(f"{bench_name:<20} {opt:<6} FORCED PL/I COMPILE FAILED")
                    results[bench_name][opt] = {"status": "pli_forced_compile_failed"}
                    continue

                # Timings are recorded, but this mode's verdict is parity.
                status, pli_times, pli_out = run_benchmark(
                    pli_exe, runs=args.runs, cwd=BIN_DIR)
                if status != "ok":
                    print(f"{bench_name:<20} {opt:<6} PL/I {status.upper()}")
                    results[bench_name][opt] = {"status": status,
                                                "flags": forced_flags_str}
                    continue
                f_status, _, pli_forced_out = run_benchmark(
                    pli_exe_forced, runs=1, cwd=BIN_DIR)
                if f_status != "ok":
                    print(f"{bench_name:<20} {opt:<6} FORCED PL/I {f_status.upper()}")
                    results[bench_name][opt] = {"status": f_status,
                                                "flags": forced_flags_str}
                    continue

                match = "✓" if pli_out.strip() == pli_forced_out.strip() else "✗"
                pli_min = min(pli_times)
                pli_median = median_result(pli_times)
                results[bench_name][opt] = {
                    "status": "ok" if match == "✓" else "output_mismatch",
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

            # Normal mode. Every measured binary is rebuilt: a cache keyed on
            # source mtimes would time a stale binary after a compiler rebuild.
            flags_str = " ".join(default_flags) if default_flags else "(default)"

            if not compile_pli(bench_name, pli_file, opt, pli_exe):
                print(f"{bench_name:<20} {opt:<6} PL/I COMPILE FAILED")
                results[bench_name][opt] = {"status": "pli_compile_failed"}
                continue
            if not compile_c(bench_name, c_file, opt, c_exe, compiler=cc_cmd):
                print(f"{bench_name:<20} {opt:<6} C COMPILE FAILED")
                results[bench_name][opt] = {"status": "c_compile_failed"}
                continue

            # Run both, each with a verified warmup, alternating the samples.
            status, pli_times, pli_out, c_status, c_times, c_out = (
                run_pair_benchmark(pli_exe, c_exe, runs=args.runs, cwd=BIN_DIR))
            if status != "ok":
                print(f"{bench_name:<20} {opt:<6} PL/I {status.upper()}")
                results[bench_name][opt] = {"status": status, "flags": flags_str}
                continue
            if c_status != "ok":
                print(f"{bench_name:<20} {opt:<6} C {c_status.upper()}")
                results[bench_name][opt] = {"status": c_status, "flags": flags_str}
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

            mark = outputs_agree(bench_name, pli_out, c_out)
            results[bench_name][opt] = {
                "status": "output_mismatch" if mark == "✗" else "ok",
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
                "match": mark,
            }
            print(f"{bench_name:<20} {opt:<6} {flags_str:<24} "
                  f"{pli_min*1000:>10.2f} {pli_median*1000:>10.2f} "
                  f"{c_median*1000:>10.2f} {ratio:>7.2f}x {mark:>6}")

    # The paired PL/I-vs-PL-I comparisons: one source compiled two ways.
    # They are a -O2 measurement (the gate the optimization plan sets), so
    # they run once, not at every optimization level. Checks-forced mode asks
    # a parity question, so the pairs would only cost time there.
    pairs = {} if args.checks_forced else run_pair_comparisons(
        pair_names, "-O2", args.runs)

    # Save JSON with machine metadata and the manifest (P3).
    manifest = build_manifest(PLI, cc_cmd, benches, opt_levels, args.runs,
                              WARMUP, args.checks_forced)
    machine_info = get_machine_info(cc_cmd)
    json_file = RESULTS_DIR / f"benchmark_results_{run_id}.json"
    payload = {"schema": SCHEMA_VERSION, "run_id": run_id, "machine": machine_info,
               "manifest": manifest, "results": results, "pairs": pairs}
    write_atomic(json_file, lambda fh: json.dump(payload, fh, indent=2))

    # Save CSV (P1.5); the status column makes a failed row explicit.
    csv_file = RESULTS_DIR / f"summary_{run_id}.csv"

    def write_csv(fh):
        w = csv.writer(fh)
        w.writerow(["benchmark", "opt", "flags", "pli_min_ms",
                    "pli_median_ms", "c_median_ms", "ratio", "match",
                    "status"])
        for bench_name, _, _ in benches:
            for opt in opt_levels:
                data = results.get(bench_name, {}).get(opt, {})
                if data.get("status") != "ok":
                    w.writerow([bench_name, opt, data.get("flags", ""), "", "",
                                "", "", "", data.get("status", "missing")])
                elif "ratio" in data:
                    pli = data["pli"]
                    c = data["c"]
                    w.writerow([bench_name, opt, data.get("flags", ""),
                                round(pli["min"] * 1000, 4),
                                round(pli["median"] * 1000, 4),
                                round(c["median"] * 1000, 4),
                                data["ratio"], data.get("match", ""), "ok"])
                else:
                    w.writerow([bench_name, opt, data.get("flags", ""), "", "",
                                "", "", data.get("parity", ""), "ok"])

    write_atomic(csv_file, write_csv)

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
            if data.get("status") != "ok":
                print(f"  {bench_name:<20} ERROR: {data.get('status', 'missing')}")
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
            if data.get("status") == "ok" and "ratio" in data:
                row += f" {data['ratio']:>7.2f}x"
            elif data.get("status") in (None, "ok"):
                row += f" {'-':>8}"
            else:
                row += f" {'ERR':>8}"
        print(row)

    # Average ratio by optimization level, per category. The ir corpus's
    # geomean is the project's acceptance gate, so the lang corpus — whose
    # benchmarks price PL/I-only costs — is reported on its own and never
    # dilutes or inflates that gate. Only valid rows are averaged, and the
    # count is printed so a partial average cannot pass for a corpus one.
    ir_names = [b[0] for b in benches if b[1].startswith("ir/")]
    lang_names = [b[0] for b in benches if b[1].startswith("lang/")]
    for label, names in (("ir (C parity — the acceptance gate)", ir_names),
                          ("lang (PL/I-specific)", lang_names)):
        if not names:
            continue
        print(f"\nAverage PL/I vs C ratio by optimization level [{label}]:")
        for opt in OPT_LEVELS:
            ratios = [results[n][opt]["ratio"] for n in names
                      if row_is_valid(results.get(n, {}).get(opt, {}))]
            if not ratios:
                print(f"  {opt}: no valid rows")
                continue
            avg = statistics.mean(ratios)
            geo = math.exp(statistics.mean(math.log(r) for r in ratios))
            print(f"  {opt}: avg ratio = {avg:.2f}x geomean = {geo:.2f}x "
                  f"(min={min(ratios):.2f}x, max={max(ratios):.2f}x, "
                  f"{len(ratios)}/{len(names)} rows valid)")

    # The acceptance gate: the full ir corpus, every row valid, at -O2.
    print("\n=== -O2 acceptance gate (ir geomean < 0.90x) ===")
    selected_gate = [n for n in GATE_BENCHMARKS if n in {b[0] for b in benches}]
    if selected_gate != GATE_BENCHMARKS:
        print(f"  not evaluated: --only selected {len(selected_gate)}/"
              f"{len(GATE_BENCHMARKS)} gate rows (a subset is a measurement, "
              "not the corpus gate)")
    else:
        gate = accepted_geomean(results, GATE_BENCHMARKS, "-O2")
        invalid = [n for n in GATE_BENCHMARKS
                   if not row_is_valid(results.get(n, {}).get("-O2", {}))
                   or results[n]["-O2"].get("match") == "~"]
        if gate is None:
            print(f"  NOT EVALUATED: {len(invalid)} of {len(GATE_BENCHMARKS)} "
                  f"rows invalid/unavailable ({', '.join(invalid[:6])}"
                  f"{'...' if len(invalid) > 6 else ''})")
        else:
            verdict = "PASS" if gate < 0.90 else "MISS"
            print(f"  {verdict}: ir geomean = {gate:.2f}x over "
                  f"{len(GATE_BENCHMARKS)} rows "
                  f"(descriptive, not gating: {', '.join(sorted(NON_PARITY))})")

    # Timing + parity validation (W7)
    print("\n=== Timing & Parity Checks (W7) ===")
    all_checks_ok = True
    for bench_name, _, _ in benches:
        chk = CHECKS.get(bench_name, {})
        min_ms = chk.get("min_ms_O2", 50)
        data_o2 = results.get(bench_name, {}).get("-O2", {})
        status = data_o2.get("status", "missing")
        if status != "ok":
            print(f"  {bench_name:<20} ✗ {status}")
            all_checks_ok = False
            continue
        pli_med = data_o2.get("pli", {}).get("median", 0) * 1000
        # Timing note (not a gate): below the noise floor the ratio is
        # indicative only. Fast benches live here *by success*, so this must
        # never fail the run — only an invalid row does that.
        if pli_med < min_ms:
            print(f"  {bench_name:<20} ◦ {pli_med:.1f}ms < {min_ms}ms noise floor", end="")
        else:
            print(f"  {bench_name:<20} ✓ {pli_med:.1f}ms ≥ {min_ms}ms", end="")
        mark = data_o2.get("match")
        if mark == "✓":
            print("  ✓ parity (exact)")
        elif mark == "≈":
            print("  ≈ parity (float-tolerant)")
        elif mark == "~":
            print(f"  ~ descriptive, not a parity twin: {NON_PARITY[bench_name]}")
        else:
            print("  ✗ parity (output mismatch)")
            all_checks_ok = False

    # Paired comparisons: a pair whose builds disagree, or that failed to
    # build or run, is a failure of the run, not a footnote.
    if pairs:
        bad_pairs = {k: v["status"] for k, v in pairs.items()
                     if v.get("status") != "ok"}
        print("\n=== Paired comparisons ===")
        if bad_pairs:
            for key, status in bad_pairs.items():
                print(f"  {key:<48} ✗ {status}")
            all_checks_ok = False
        else:
            print(f"  ✓ {len(pairs)} pair(s) valid and output-identical")

    # Regression guard: only against a run whose settings match this one.
    prev_full = load_previous_results(json_file.name, args.compare_to)
    previous = prev_full.get("results") if isinstance(prev_full, dict) else None
    print("\n=== REGRESSION CHECK ===")
    if previous is None:
        print("  — No previous results found to compare against")
    else:
        reason = manifest_mismatch(manifest, prev_full)
        if reason:
            print(f"  — Not compared: {reason}")
        else:
            prev_id = prev_full.get("machine", {}).get("compiler_id")
            cur_id = machine_info.get("compiler_id")
            if prev_id and cur_id and prev_id != cur_id:
                print(f"  Compiler changed ({prev_id} → {cur_id}); ratio deltas "
                      "mix the change with noise.")
            # Pinning changes the noise floor, not the workload: say so rather
            # than let a new pinning mode masquerade as a codegen delta.
            prev_pin = (prev_full.get("manifest") or {}).get("pinning", {})
            if prev_pin.get("mode") != manifest["pinning"]["mode"]:
                print(f"  Pinning changed ({prev_pin.get('mode')} → "
                      f"{manifest['pinning']['mode']}); treat small deltas with care.")
            warnings, compared = check_for_deterioration(
                results, previous, [b[0] for b in benches])
            if warnings:
                print(f"  ⚠ {len(warnings)} benchmark(s) deteriorated by >"
                      f"{int(DETERIORATION_THRESHOLD*100)}% at -O2 "
                      f"(compared {compared} valid rows)")
                for w in warnings:
                    print(w)
            else:
                print(f"  ✓ No material deterioration at -O2 vs previous run "
                      f"(compared {compared} valid rows)")

    return 0 if all_checks_ok else 1


if __name__ == "__main__":
    sys.exit(main())
