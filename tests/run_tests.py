#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
"""tests/run_tests.py — compile, run and check every test program, in parallel.

Usage: tests/run_tests.py [group ... | <group>/<name>.pli|.sh ...]
Without arguments every tests/*/ group runs; otherwise only the named groups,
or the single tests named by path (e.g. usecases/data.pli).

Each tests/<group>/ subfolder is a test group containing:
  <group>/*.pli           test programs (bad_*.pli must be rejected)
  <group>/*.sh            driver tests (compile and check several programs)
  <group>/expected/*.out  recorded stdout for golden tests
  <group>/out/            scratch binaries, logs and diffs (gitignored)
A group may mix test classes; each test is classified individually:
  golden — expected/<name>.out exists: stdout is diff-checked against it
  self   — no expected/<name>.out: the program verifies itself and must print
           PASS (case-insensitive); any FAIL in its output fails it

A test that really ran under a sanitizer prints `SANITIZER: <list>`; the run
ends with the coverage of every group and counts the tests that SKIPped.
Tests run concurrently, bounded by JOBS (default: number of CPUs). Results are
printed in deterministic group order. `cmake --build -j` parallelises the
compile step.
"""
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Optional

TIMEOUT = 30  # seconds per exec run step
# The ceiling a load-scaled exec budget may reach.
EXEC_TIMEOUT_MAX = 120
# Driver scripts compile and link several programs each, so one short budget
# starves them under parallel load (the overflow drivers timed out cold);
# they get their own budget while single-binary runs stay tight enough to
# catch genuine hangs quickly.
DRIVER_TIMEOUT = 120  # seconds per driver test
# The ceiling the load-scaled driver budget may reach.
DRIVER_TIMEOUT_MAX = 300
# Sanitizer drivers rebuild and instrument the runtime, so they need a larger
# floor than an ordinary driver even on an idle host.
SANITIZER_TIMEOUT = 240
# The fuzz driver compiles ten programs twice; it shares the machine with the
# sanitizer builds and needs a larger floor under that load.
FUZZ_TIMEOUT = 180


def _host_load() -> float:
    """The host's load average, or its CPU busy fraction scaled by core count.

    `os.getloadavg()` is Unix-only, so on Windows the load-scaled timeouts
    below silently fell back to their flat floor and a slow-but-correct
    driver was reported as a failure. Windows has no load average, but
    `GetSystemTimes` reports the idle and busy kernel/user times, so the
    same quantity - the number of cores' worth of work in flight - is
    recovered from the busy fraction. Returns 0.0 when neither probe works.
    """
    try:
        return float(os.getloadavg()[0])
    except (AttributeError, OSError):
        pass
    if not IS_WINDOWS:
        return 0.0
    try:
        import ctypes
        from ctypes import wintypes

        class FILETIME(ctypes.Structure):
            _fields_ = [("dwLowDateTime", wintypes.DWORD),
                        ("dwHighDateTime", wintypes.DWORD)]

        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        idle, kernel, user = FILETIME(), FILETIME(), FILETIME()

        def stamp():
            k32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kernel),
                               ctypes.byref(user))
            return (idle.dwHighDateTime << 32) | idle.dwLowDateTime, \
                   (kernel.dwHighDateTime << 32) | kernel.dwLowDateTime, \
                   (user.dwHighDateTime << 32) | user.dwLowDateTime

        i0, k0, u0 = stamp()
        time.sleep(0.2)
        i1, k1, u1 = stamp()
        total = (k1 - k0) + (u1 - u0)
        if total <= 0:
            return 0.0
        busy = total - (i1 - i0)
        return max(0.0, busy / total) * (os.cpu_count() or 1)
    except Exception:
        return 0.0


# A fixed CPU-bound workload used to measure how much slower the host runs
# under contention than when idle. The loop count is small enough to keep the
# probe cheap and large enough for the wall-clock reading to be stable.
_PROBE_ITERS = 150000
# The probe's fastest observed time: the least-contended reading, which is
# the fair baseline to compare a later reading against. Set from an idle
# host at start-up, then lowered if a quieter moment is ever observed.
_probe_baseline: Optional[float] = None


def _probe_seconds() -> float:
    """Wall-clock seconds for the fixed CPU-bound workload."""
    t0 = time.perf_counter()
    x = 0
    for i in range(_PROBE_ITERS):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
    del x
    return time.perf_counter() - t0


def _contention_factor() -> float:
    """How much slower a CPU-bound task runs now than on an idle host.

    The load average counts runnable tasks, so on Unix it already exceeds
    the core count when the machine is oversubscribed and the load-scaled
    budgets grow on their own. Windows has no load average, and the busy
    fraction recovered above saturates at the core count, so it cannot show
    oversubscription: a box running one task per core looks identical to a
    saturated one. This probe measures the effect directly instead - the
    same fixed workload takes proportionally longer when the scheduler
    cannot give the process a full core - so the budget grows by the
    factor the run actually experiences.
    """
    global _probe_baseline
    now = _probe_seconds()
    if _probe_baseline is None or now < _probe_baseline:
        _probe_baseline = now
    if not _probe_baseline:
        return 1.0
    return max(1.0, now / _probe_baseline)


def exec_timeout() -> int:
    """The per-run budget for a compiled test, scaled by host load.

    A test that finishes in milliseconds can wait behind the rest of the
    suite on a busy host; the fixed budget then fails a correct program.
    The load average is sampled once per test, so an idle host keeps the
    tight budget that catches a genuine hang.
    """
    budget = TIMEOUT
    load = _host_load()
    if load > 0:
        cores = os.cpu_count() or 1
        budget = max(budget, int(TIMEOUT * load / cores))
    budget = int(budget * _contention_factor())
    return min(EXEC_TIMEOUT_MAX, max(TIMEOUT, budget))


def driver_timeout() -> int:
    """The driver budget, scaled by how busy the host is.

    The budget is wall-clock, so on a shared machine a fixed one fails a
    test that is merely slow: the drivers that link and run a program are
    the first to exceed it, and the whole suite then reports a failure
    that no amount of rerunning explains. The load average is sampled
    once, at start-up, and the budget grows with it. On an idle host it
    stays at DRIVER_TIMEOUT, so a genuine hang is still caught quickly.
    """
    budget = DRIVER_TIMEOUT
    load = _host_load()
    if load > 0:
        cores = os.cpu_count() or 1
        budget = max(budget, int(DRIVER_TIMEOUT * load / cores))
    budget = int(budget * _contention_factor())
    return min(DRIVER_TIMEOUT_MAX, max(DRIVER_TIMEOUT, budget))
ROOT = Path(__file__).resolve().parent.parent
IS_WINDOWS = sys.platform == "win32"
EXE_EXT = ".exe" if IS_WINDOWS else ""


def _default_pli():
    # CTest passes PLI explicitly ($<TARGET_FILE:pli>); this default only
    # matters for manual runs. Prefer the CMake layout (build/cmake) when it
    # exists, falling back to the legacy build/ path.
    for cand in (ROOT / "build" / "cmake" / f"pli{EXE_EXT}",
                 ROOT / "build" / f"pli{EXE_EXT}"):
        if cand.exists():
            return str(cand)
    return str(ROOT / "build" / "cmake" / f"pli{EXE_EXT}")


PLI = os.environ.get("PLI", _default_pli())
CLANG = os.environ.get("CLANG", "clang")
TYPES = ("driver", "exec", "diag", "ir")


def run_cmd(cmd, outfile, timeout=TIMEOUT):
    """Run `cmd`, appending stdout+stderr to `outfile`; kill the process
    if it exceeds `timeout`. Return (returncode, timeout_bool) — 124 on timeout."""
    # Popen (not run) so the timeout path still owns the child handle: `run`
    # raises before binding its result, which crashed the whole suite instead
    # of failing the one slow job.
    with open(outfile, "wb") as fh:
        # start_new_session is POSIX-only; on Windows the default process
        # group is fine since we kill the direct child via proc.kill().
        popen_kwargs = {} if IS_WINDOWS else {"start_new_session": True}
        proc = subprocess.Popen(cmd, stdout=fh, stderr=subprocess.STDOUT,
                                **popen_kwargs)
        try:
            proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            try:
                # Portable kill: os.killpg/getpgid do not exist on Windows.
                proc.kill()
            except (ProcessLookupError, PermissionError):
                pass
            proc.wait()
            return 124, True
        return proc.returncode, False


def indented(text):
    return "\n".join("      " + line for line in text.rstrip("\n").splitlines())


def ir_match(ir, checkfile):
    """FileCheck-style ordered match. Each `CHECK:`/`CHECK-LABEL:` regex line
    must match, in order, somewhere after the previous positive match in `ir`;
    `CHECK-NOT:` lines assert their pattern does not appear in the window
    between the surrounding positive matches. A FileCheck regex block
    `{{...}}` becomes a Python group `(?:...)`; the rest of the line is a
    Python regex (so a literal parenthesis is backslash escaped in the check
    file). Returns (ok, reason). Minimal and dependency-free (no FileCheck
    binary)."""
    dirs = []
    for lineno, line in enumerate(checkfile.splitlines(), 1):
        s = line.strip()
        if not s:
            continue
        kind = None
        for k in ("CHECK-LABEL:", "CHECK-NOT:", "CHECK:"):
            if line.startswith(k):
                kind = k
                break
        if kind is None:
            return False, f".check line {lineno}: expected 'CHECK:' directive"
        pat = line[len(kind):].strip()
        if not pat:
            return False, f".check line {lineno}: empty CHECK pattern"
        pat = re.sub(r"\{\{(.*?)\}\}", r"(?:\1)", pat)
        try:
            re.compile(pat)
        except re.error as e:
            return False, f".check line {lineno}: bad pattern '{pat}': {e}"
        dirs.append((kind, pat, lineno))

    # Walk positive matches, enforcing each CHECK-NOT against the window from
    # the previous positive match to the current one.
    pos = 0
    pending_not = []  # (pattern, lineno) of CHECK-NOT lines seen since last positive
    for kind, pat, lineno in dirs:
        if kind in ("CHECK:", "CHECK-LABEL:"):
            m = re.search(pat, ir[pos:], re.MULTILINE)
            if not m:
                return False, f".check line {lineno}: pattern not found: {pat}"
            end = pos + m.end()
            for npat, nline in pending_not:
                if re.search(npat, ir[pos:end], re.MULTILINE):
                    return False, f".check line {nline}: pattern should not appear: {npat}"
            pending_not = []
            pos = end
        else:  # CHECK-NOT
            pending_not.append((pat, lineno))
    return True, ""


class Runner:
    def __init__(self, dir_path, name):
        self.dir = Path(dir_path)
        self.name = name
        self.out = self.dir / "out"
        self.out.mkdir(parents=True, exist_ok=True)
        # Sanitizers this test really ran under (from its `SANITIZER:` line).
        self.sanitizers = set()

    def fail(self, reason, detail=""):
        """Uniform failure line `FAIL <name>: <reason>`, with any detail
        (diff, compiler output, test output) indented beneath it."""
        head = f"FAIL {self.name}: {reason}"
        return (head + ("\n" + indented(detail) if detail else ""), 1)

    def check_out(self):
        """Classify a produced <name>.out: golden diff or self-check PASS."""
        outfile = self.out / f"{self.name}.out"
        text = outfile.read_text(errors="replace") if outfile.exists() else ""
        # A test that really ran under a sanitizer prints `SANITIZER: <list>`;
        # main() aggregates these into the per-group coverage report (B0).
        for line in text.splitlines():
            if line.startswith("SANITIZER:"):
                for s in line.split(":", 1)[1].replace(",", " ").split():
                    self.sanitizers.add(s)
        expected = self.dir / "expected" / f"{self.name}.out"
        if expected.exists():
            # Portable unified diff: the external `diff` utility is not
            # available on Windows, so use difflib on both platforms for
            # identical output.
            import difflib
            exp_lines = expected.read_text(errors="replace").splitlines()
            out_lines = text.splitlines()
            diff = list(difflib.unified_diff(
                exp_lines, out_lines,
                fromfile=str(expected), tofile=str(outfile), lineterm=""))
            if not diff:
                return f"PASS {self.name}", 0
            return self.fail("output differs", chr(10).join(diff))
        if "PASS" in text.upper() and "FAIL" not in text.upper():
            return f"PASS {self.name}", 0
        # A script may SKIP itself (e.g. toolchain lacks a feature); a skip
        # with no failure is a pass-equivalent, reported distinctly.
        if text.lstrip().startswith("SKIP") and "FAIL" not in text.upper():
            first = text.strip().splitlines()[0]
            reason = first[4:].strip().lstrip(":").strip() or "skipped"
            return f"SKIP {self.name} ({reason})", 0
        # Surface the test's own diagnostic on the FAIL line, so the reason is
        # visible without reading the captured output.
        for line in text.splitlines():
            if re.search(r"\bFAIL\b", line, re.I):
                reason = re.sub(r"^\s*FAIL\b[:\s]*", "", line.strip(), flags=re.I)
                return self.fail(reason or "test failed", text)
        return self.fail("self test did not print PASS", text or "(no output)")

    def driver(self):
        import shutil
        sh = shutil.which("sh")
        if sh is None:
            # Git for Windows ships sh.exe outside the default PATH.
            for cand in (r"C:\Program Files\Git\bin\sh.exe",
                         r"C:\Program Files (x86)\Git\bin\sh.exe",
                         r"C:\Program Files\Git\usr\bin\sh.exe",
                         r"C:\Program Files (x86)\Git\usr\bin\sh.exe",
                         r"C:\Git\bin\sh.exe",
                         r"C:\Git\usr\bin\sh.exe"):
                if Path(cand).exists():
                    sh = cand
                    break
        if sh is None:
            return self.fail("driver tests need a POSIX shell (sh not found)")
        # Forward the resolved tool paths: driver scripts default to
        # ./build/cmake/pli, so the env must carry the real (possibly .exe) path.
        import os as _os
        env = dict(_os.environ, PLI=PLI, CLANG=CLANG)
        budget = driver_timeout()
        if self.name in ("storage_asan", "tasking_tsan"):
            budget = max(budget, SANITIZER_TIMEOUT)
        elif self.name == "peephole_fuzz":
            budget = max(budget, FUZZ_TIMEOUT)
        try:
            with open(self.out / f"{self.name}.out", "wb") as fh:
                proc = subprocess.Popen([sh, str(self.dir / f"{self.name}.sh")],
                                        stdout=fh, stderr=subprocess.STDOUT,
                                        cwd=str(ROOT), env=env)
                try:
                    proc.wait(timeout=budget)
                except subprocess.TimeoutExpired:
                    try:
                        proc.kill()
                    except (ProcessLookupError, PermissionError):
                        pass
                    proc.wait()
                    return self.fail(f"timed out after {budget}s")
                rc = proc.returncode
        except FileNotFoundError as e:
            return self.fail(f"cannot run shell: {e}")
        if rc == 124:
            return self.fail(f"timed out after {budget}s")
        return self.check_out()

    def exec_test(self):
        src = self.dir / f"{self.name}.pli"
        libsrc = self.dir / f"{self.name}_lib.pli"
        compilefile = self.out / f"{self.name}.compile"
        with open(compilefile, "w") as cfh:
            if libsrc.exists() or (self.dir / f"{self.name}.c").exists():
                # Cross-unit test: compile each unit and link with the runtime.
                # A `_lib.pli` companion is a PL/I module (multimodule group);
                # a `.c` companion is a C translation unit.
                objs = []
                steps = []
                if libsrc.exists():
                    objs.append(self.out / f"{self.name}_lib.pli.o")
                    steps.append([PLI, "--size-checks", str(libsrc), "-c", "-o", str(objs[-1])])
                if (self.dir / f"{self.name}.c").exists():
                    objs.append(self.out / f"{self.name}.c.o")
                    steps.append([CLANG, "-c", str(self.dir / f"{self.name}.c"),
                                  "-o", str(objs[-1])])
                objs.append(self.out / f"{self.name}.pli.o")
                steps.append([PLI, "--size-checks", str(src), "-c", "-o", str(objs[-1])])
                # Link through the pli driver (embedded pli_runtime): clang is
                # only the `.c` compiler above, never the linker here.
                steps.append([PLI, "--size-checks"] + [str(o) for o in objs] +
                               ["-o", str(self.out / (self.name + EXE_EXT))])
                failed = None
                for step in steps:
                    if subprocess.run(step, stdout=cfh, stderr=subprocess.STDOUT).returncode != 0:
                        failed = "cross-unit build failed"
                        break
            else:
                if subprocess.run([PLI, "--size-checks", str(src), "-o", str(self.out / (self.name + EXE_EXT))],
                                  stdout=cfh, stderr=subprocess.STDOUT).returncode != 0:
                    failed = "compilation failed"
                else:
                    failed = None
        if failed:
            return self.fail(failed,
                             compilefile.read_text(errors="replace"))
        budget = exec_timeout()
        rc, _ = run_cmd([str(self.out / (self.name + EXE_EXT))],
                        self.out / f"{self.name}.out", timeout=budget)
        if rc == 124:
            return self.fail(f"timed out after {budget}s")
        return self.check_out()

    def diag(self):
        compilefile = self.out / f"{self.name}.compile"
        rc = subprocess.run([PLI, str(self.dir / f"{self.name}.pli"), "-fsyntax-only"],
                            stdout=open(compilefile, "w"),
                            stderr=subprocess.STDOUT).returncode
        text = compilefile.read_text(errors="replace") if compilefile.exists() else ""
        if rc == 0:
            return self.fail("expected diagnostics but compiled cleanly", text)
        # Optional FileCheck-style assertion over the combined compiler output
        # (reuses the ir matcher). Lets a diag test pin source location lines
        # such as file:line:col without asserting message wording.
        checkfile = self.dir / "expected" / f"{self.name}.check"
        if checkfile.exists():
            ok, reason = ir_match(text, checkfile.read_text(errors="replace"))
            if not ok:
                return self.fail(f"check: {reason}", text)
        return f"PASS {self.name} (rejected as expected)", 0

    def ir_test(self):
        # Emit LLVM IR for the program and FileCheck it against <name>.check.
        checkfile = self.dir / f"{self.name}.check"
        if not checkfile.exists():
            return self.fail(f"missing {self.name}.check")
        llfile = self.out / f"{self.name}.ll"
        compilefile = self.out / f"{self.name}.compile"
        # Optional per-test compiler flags (e.g. --experimental-lowering=llvm).
        flagsfile = self.dir / f"{self.name}.flags"
        flags = []
        if flagsfile.exists():
            flags = flagsfile.read_text(errors="replace").strip().split()
        with open(compilefile, "w") as cfh:
            rc = subprocess.run(
                [PLI, "--size-checks", *flags, str(self.dir / f"{self.name}.pli"),
                 "-emit-llvm", "-o", str(llfile)],
                stdout=cfh, stderr=subprocess.STDOUT).returncode
        if rc != 0:
            return self.fail("compilation failed",
                             compilefile.read_text(errors="replace"))
        ir = llfile.read_text(errors="replace")
        ok, reason = ir_match(ir, checkfile.read_text(errors="replace"))
        if not ok:
            return self.fail(f"IR check: {reason}", ir)
        return f"PASS {self.name}", 0

    def run(self):
        fn = {"driver": self.driver, "exec": self.exec_test,
              "diag": self.diag, "ir": self.ir_test}.get(self.kind)
        return fn()


def enumerate_jobs(args):
    """Return the ordered list of (dir, name, kind) jobs selected by `args`."""
    groups, onetest = [], None
    for arg in args:
        if arg.endswith(".pli") or arg.endswith(".sh"):
            t = arg[2:] if arg.startswith("./") else arg
            t = t if t.startswith("tests/") else f"tests/{t}"
            p = Path(t)
            if not p.is_file():
                sys.exit(f"run_tests.py: no such test: {arg}")
            onetest = (p.parent, p.stem)
        else:
            d = Path("tests") / arg
            if not d.is_dir():
                sys.exit(f"run_tests.py: no such test group: tests/{arg}")
            groups.append(arg)

    jobs = []
    for dir_path in sorted(Path("tests").iterdir()):
        if not dir_path.is_dir():
            continue
        base = dir_path.name
        if base in groups:
            single = None
        elif onetest and dir_path == onetest[0]:
            single = onetest[1]
        elif groups:
            continue
        elif not onetest:
            single = None
        else:
            continue
        has = (list(dir_path.glob("*.pli")) or list(dir_path.glob("*.sh")))
        if not has:
            continue

        def add(name, kind):
            if single is not None and name != single:
                return
            jobs.append((str(dir_path), name, kind))

        for f in sorted(dir_path.glob("*.sh")):
            add(f.stem, "driver")
        for f in sorted(dir_path.glob("*.pli")):
            # A `_lib.pli` file is a companion unit of a multimodule test,
            # not a test of its own.
            if f.name.endswith("_lib.pli"):
                continue
            if not f.name.startswith("bad_"):
                # tests/ir/*.pli are IR golden tests (FileCheck against .check).
                kind = "ir" if dir_path.name == "ir" else "exec"
                add(f.stem, kind)
        for f in sorted(dir_path.glob("bad_*.pli")):
            add(f.stem, "diag")
    return jobs


def main():
    jobs = enumerate_jobs(sys.argv[1:])
    ncores = os.cpu_count() or 4
    workers = int(os.environ.get("JOBS", str(ncores)))

    # Prime the contention baseline before the suite starts loading the
    # machine: the first probe of the run is the closest thing to an idle
    # reading, and every later probe is compared against it.
    global _probe_baseline
    _probe_baseline = _probe_seconds()

    # Tests that cannot run on Windows for platform reasons (not code bugs):
    # - builtins/system shells out to the Unix `true`/`false` commands.
    # - core/socket_pair needs <sys/socket.h>, absent on Windows.
    # - driver/task_record needs a POSIX FIFO a native program can open.
    WINDOWS_SKIP = {
        ("tests/builtins", "system"): "needs Unix true/false commands",
        ("tests/core", "socket_pair"): "needs <sys/socket.h>",
        ("tests/driver", "task_record"): "needs a POSIX FIFO",
    }

    # Tests that need a C compiler for the C side of the interop. They are
    # skipped only when no C compiler is available: on a box with clang (or
    # gcc) in PATH they run, so the coverage does not depend on the machine.
    NEEDS_CC_SKIP = {
        ("tests/core", "c_struct_return"),
        ("tests/core", "cbyvalue"),
        ("tests/core", "cinterop"),
        ("tests/core", "c_call_pli_abi"),
        ("tests/core", "entry_mixed"),
        ("tests/driver", "link"),
        ("tests/driver", "multic"),
        ("tests/driver", "multimod"),
        ("tests/driver", "package"),
        ("tests/driver", "rt_platform"),
        ("tests/driver", "struct_mirror"),
        ("tests/driver", "winsock"),
    }

    def have_c_compiler() -> bool:
        """Whether a C compiler is available for the C side of the interop."""
        import shutil
        for cand in (CLANG, "clang", "clang-cl", "gcc", "cc"):
            if cand and shutil.which(cand):
                return True
        return False

    cc_available = have_c_compiler()

    def work(job):
        dir_path, name, kind = job
        print(f"RUN {name}", flush=True)
        # dir_path may use native separators; normalize for the skip lookup.
        if IS_WINDOWS:
            key = (Path(dir_path).as_posix(), name)
            if key in WINDOWS_SKIP:
                return job, f"SKIP {name} ({WINDOWS_SKIP[key]})", 0, ()
            if key in NEEDS_CC_SKIP and not cc_available:
                return job, f"SKIP {name} (needs clang in PATH)", 0, ()
        r = Runner(dir_path, name)
        r.kind = kind
        result, rc = r.run()
        return job, result, rc, tuple(sorted(r.sanitizers))

    results = {}
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = {pool.submit(work, job): job for job in jobs}
        for fut in as_completed(futures):
            job, result, rc, sans = fut.result()
            print(result, flush=True)
            results[job] = (result, rc, sans)

    pass_count = fail_count = skip_count = 0
    coverage = {}
    for job in jobs:
        result, rc, sans = results[job]
        print(result)
        if rc == 0 and result.startswith("SKIP "):
            skip_count += 1
        elif rc == 0:
            pass_count += 1
        else:
            fail_count += 1
        # Count each group's tests per sanitizer set; an unmarked test ran none.
        group = Path(job[0]).name
        per = coverage.setdefault(group, {})
        key = ",".join(sans) if sans else "none"
        per[key] = per.get(key, 0) + 1

    print(f"\n{pass_count} passed, {fail_count} failed, {skip_count} skipped")
    # B0: a test only claims a sanitizer when it actually ran under one, so
    # this table is the measured coverage of the run, not an intention.
    print("sanitizer coverage by group:")
    for group in sorted(coverage):
        parts = ", ".join(f"{s} ({n})" for s, n in sorted(coverage[group].items()))
        print(f"  {group}: {parts}")
    return 0 if fail_count == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
