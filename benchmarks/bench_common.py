#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
"""Shared execution primitives for the benchmark harnesses.

A timing sample is admitted only when the process completed successfully and
printed the output it was expected to print. A crash, a timeout, a missing
binary or a silent early exit therefore becomes an explicit status, never a
plausible-looking ratio.
"""
import hashlib
import os
import platform
import shutil
import subprocess
import time

IS_WINDOWS = os.name == "nt"
DEFAULT_TIMEOUT = 60


class MeasureError(RuntimeError):
    """A run that cannot be admitted as a timing sample."""

    def __init__(self, status: str, detail: str = ""):
        super().__init__(detail or status)
        self.status = status
        self.detail = detail


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


def _probe_prefix(prefix) -> tuple:
    """Run `prefix + true` and report whether the wrapper really took effect.

    `nice` exits 0 after failing to raise its priority on macOS, so exit status
    alone is not evidence that pinning happened; the diagnostic is checked too.
    """
    try:
        proc = subprocess.run(list(prefix) + ["true"], capture_output=True,
                              text=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return False, str(exc)
    if proc.returncode != 0:
        return False, (proc.stderr or "").strip() or f"exit {proc.returncode}"
    lowered = (proc.stderr or "").lower()
    if "permission denied" in lowered or "not permitted" in lowered:
        return False, proc.stderr.strip()
    return True, ""


def _linux_cpu() -> int:
    """First CPU this process is allowed to use (CPU 0 may not be in the set)."""
    try:
        allowed = sorted(os.sched_getaffinity(0))
    except AttributeError:
        allowed = []
    return allowed[0] if allowed else 0


def probe_pinning() -> dict:
    """Resolve the process-pinning prefix once, recording whether it applied.

    The outcome is recorded in every result file: a run whose priority was
    silently refused is not the same measurement as one that was pinned.
    """
    if IS_WINDOWS:
        # run_measured applies affinity/priority to the child handle itself;
        # there is no argv wrapper for that, so `prefix` stays empty.
        return {"mode": "windows-affinity", "prefix": [], "applied": True,
                "note": "affinity and HIGH_PRIORITY_CLASS set on the child"}
    if platform.system() == "Darwin":
        prefix = ["nice", "-n", "-20"]
        applied, detail = _probe_prefix(prefix)
        if applied:
            return {"mode": "nice", "prefix": prefix, "applied": True,
                    "note": "nice -n -20 accepted"}
        return {"mode": "unpinned", "prefix": [], "applied": False,
                "note": f"nice -n -20 refused ({detail}); running unpinned"}
    if shutil.which("taskset") is None:
        return {"mode": "unpinned", "prefix": [], "applied": False,
                "note": "taskset unavailable"}
    cpu = _linux_cpu()
    prefix = ["taskset", "-c", str(cpu)]
    applied, detail = _probe_prefix(prefix)
    if applied:
        return {"mode": "taskset", "prefix": prefix, "applied": True,
                "note": f"pinned to allowed CPU {cpu}"}
    return {"mode": "unpinned", "prefix": [], "applied": False,
            "note": f"taskset refused ({detail}); running unpinned"}


def run_measured(cmd, *, env=None, cwd=None, timeout: int = DEFAULT_TIMEOUT,
                 expected_stdout=None, prefix=()) -> tuple:
    """Run *cmd* once and return (seconds, stdout).

    Raises MeasureError with a status of spawn_failed, run_timeout, run_failed
    or output_mismatch. Wall time is sampled around the whole process, so the
    caller must account for startup when the workload is short.
    """
    argv = list(prefix) + [str(c) for c in cmd]
    start = time.perf_counter()
    try:
        proc = subprocess.Popen(argv, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, cwd=cwd, env=env,
                                text=True, encoding="utf-8")
    except OSError as exc:
        raise MeasureError("spawn_failed", f"{argv[0]}: {exc}")
    if IS_WINDOWS:
        _pin_child_windows(proc)
    try:
        out, err = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.communicate()
        raise MeasureError("run_timeout", f"{argv[0]} exceeded {timeout}s")
    elapsed = time.perf_counter() - start
    if proc.returncode != 0:
        raise MeasureError("run_failed",
                           f"{argv[0]} exited {proc.returncode}: "
                           f"{(err or '').strip()}")
    if expected_stdout is not None and out.strip() != expected_stdout:
        raise MeasureError(
            "output_mismatch",
            f"{argv[0]} printed {out.strip()!r}, expected {expected_stdout!r}")
    return elapsed, out


def sha256_file(path) -> str:
    """Content hash of a file, used to key rebuild decisions on what ran."""
    digest = hashlib.sha256()
    try:
        with open(path, "rb") as fh:
            for block in iter(lambda: fh.read(1 << 20), b""):
                digest.update(block)
    except OSError:
        return "missing"
    return digest.hexdigest()
