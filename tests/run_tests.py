#!/usr/bin/env python3
"""tests/run_tests.py — compile, run and check every test program, in parallel.

Usage: tests/run_tests.py [group ... | <group>/<name>.pli ...]
Without arguments every tests/*/ group runs; otherwise only the named groups,
or the single tests named by path (e.g. usecases/data.pli).

Each tests/<group>/ subfolder is a test group containing:
  <group>/*.pli           test programs (bad_*.pli must be rejected)
  <group>/expected/*.out  recorded stdout for golden tests
  <group>/out/            scratch binaries, logs and diffs (gitignored)
A group may mix test classes; each test is classified individually:
  golden — expected/<name>.out exists: stdout is diff-checked against it
  self   — no expected/<name>.out: the program verifies itself and must print
           PASS (case-insensitive); any FAIL in its output fails it

Tests run concurrently, bounded by JOBS (default: number of CPUs). Results are
printed in deterministic group order. `make -j` parallelises the compile step.
"""
import os
import signal
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

TIMEOUT = 10  # seconds per test
ROOT = Path(__file__).resolve().parent.parent
PLIC = os.environ.get("PLIC", str(ROOT / "build" / "plic"))
RTLIB = os.environ.get("RTLIB", str(ROOT / "build" / "libpli.a"))
CLANG = os.environ.get("CLANG", "clang")
TYPES = ("driver", "exec", "diag")


def run_cmd(cmd, outfile):
    """Run `cmd`, appending stdout+stderr to `outfile`; kill the process group
    if it exceeds TIMEOUT. Return (returncode, timeout_bool) — 124 on timeout."""
    try:
        with open(outfile, "wb") as fh:
            p = subprocess.run(cmd, stdout=fh, stderr=subprocess.STDOUT,
                               start_new_session=True, timeout=TIMEOUT)
        return p.returncode, False
    except subprocess.TimeoutExpired:
        try:
            os.killpg(os.getpgid(p.pid), signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass
        return 124, True


def indented(text):
    return "\n".join("      " + line for line in text.rstrip("\n").splitlines())


class Runner:
    def __init__(self, dir_path, name):
        self.dir = Path(dir_path)
        self.name = name
        self.out = self.dir / "out"
        self.out.mkdir(parents=True, exist_ok=True)

    def check_out(self):
        """Classify a produced <name>.out: golden diff or self-check PASS."""
        outfile = self.out / f"{self.name}.out"
        expected = self.dir / "expected" / f"{self.name}.out"
        if expected.exists():
            diff = subprocess.run(["diff", "-u", str(expected), str(outfile)],
                                  capture_output=True, text=True)
            if diff.returncode == 0:
                return f"PASS {self.name}", 0
            return f"FAIL {self.name} (output differs)\n{indented(diff.stdout)}", 1
        text = outfile.read_text(errors="replace") if outfile.exists() else ""
        if "PASS" in text.upper() and "FAIL" not in text.upper():
            return f"PASS {self.name}", 0
        return f"FAIL {self.name} (self test did not print PASS)\n\n{indented(text)}", 1

    def driver(self):
        rc, _ = run_cmd(["sh", str(self.dir / f"{self.name}.sh")],
                        self.out / f"{self.name}.out")
        if rc == 124:
            return f"FAIL {self.name} (timed out after {TIMEOUT}s)\n", 1
        return self.check_out()

    def exec_test(self):
        src = self.dir / f"{self.name}.pli"
        compilefile = self.out / f"{self.name}.compile"
        with open(compilefile, "w") as cfh:
            if (self.dir / f"{self.name}.c").exists():
                # Cross-unit test: compile each unit and link with the runtime.
                steps = [
                    [CLANG, "-c", str(self.dir / f"{self.name}.c"),
                     "-o", str(self.out / f"{self.name}.c.o")],
                    [PLIC, str(src), "-c", "-o", str(self.out / f"{self.name}.pli.o")],
                    [CLANG, str(self.out / f"{self.name}.pli.o"),
                     str(self.out / f"{self.name}.c.o"), RTLIB,
                     "-o", str(self.out / self.name)],
                ]
                failed = None
                for step in steps:
                    if subprocess.run(step, stdout=cfh, stderr=subprocess.STDOUT).returncode != 0:
                        failed = "cross-unit build failed"
                        break
            else:
                if subprocess.run([PLIC, str(src), "-o", str(self.out / self.name)],
                                  stdout=cfh, stderr=subprocess.STDOUT).returncode != 0:
                    failed = "compilation failed"
                else:
                    failed = None
        if failed:
            return (f"FAIL {self.name} ({failed})\n"
                    f"{indented(compilefile.read_text(errors='replace'))}"), 1
        rc, _ = run_cmd([str(self.out / self.name)], self.out / f"{self.name}.out")
        if rc == 124:
            return f"FAIL {self.name} (timed out after {TIMEOUT}s)\n", 1
        return self.check_out()

    def diag(self):
        compilefile = self.out / f"{self.name}.compile"
        rc = subprocess.run([PLIC, str(self.dir / f"{self.name}.pli"), "-fsyntax-only"],
                            stdout=open(compilefile, "w"),
                            stderr=subprocess.STDOUT).returncode
        if rc == 0:
            return f"FAIL {self.name} (expected diagnostics, compiled cleanly)", 1
        return f"PASS {self.name} (rejected as expected)", 0

    def run(self):
        fn = {"driver": self.driver, "exec": self.exec_test,
              "diag": self.diag}.get(self.kind)
        return fn()


def enumerate_jobs(args):
    """Return the ordered list of (dir, name, kind) jobs selected by `args`."""
    groups, onetest = [], None
    for arg in args:
        if arg.endswith(".pli"):
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
            if not f.name.startswith("bad_"):
                add(f.stem, "exec")
        for f in sorted(dir_path.glob("bad_*.pli")):
            add(f.stem, "diag")
    return jobs


def main():
    jobs = enumerate_jobs(sys.argv[1:])
    ncores = os.cpu_count() or 4
    workers = int(os.environ.get("JOBS", str(ncores)))

    def work(job):
        dir_path, name, kind = job
        r = Runner(dir_path, name)
        r.kind = kind
        result, rc = r.run()
        return job, result, rc

    results = {}
    with ThreadPoolExecutor(max_workers=workers) as pool:
        for job, result, rc in pool.map(work, jobs):
            results[job] = (result, rc)

    pass_count = fail_count = 0
    for job in jobs:
        result, rc = results[job]
        print(result)
        pass_count += rc == 0
        fail_count += rc != 0
    print(f"\n{pass_count} passed, {fail_count} failed")
    return 0 if fail_count == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
