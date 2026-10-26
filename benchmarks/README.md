# Benchmarks

Two self-contained corpora plus a tasking harness. Each workload is a PL/I
program and a C program that does the same work; a few also have a Go twin.

| Path | What it is | Gate |
|---|---|---|
| `ir/` | Generic workloads whose PL/I and C twins do the same thing. | The `-O2` geomean (`pli/c` ratio) is the project's acceptance gate: `< 0.90x`. |
| `lang/` | Workloads that exercise PL/I semantics a C program cannot express. | Paired PL/I-vs-PL/I builds (`PAIRS` in the runner); the C twin is a reference only. |
| `tasking/` | Task lifecycle and fan-out, PL/I against Go and C. | Fiber model `<= 0.90x` Go on `task_loop`. |
| `tests/` | Harness self-tests (no compiler needed). | Run with the suite: `tests/driver/bench_harness.sh`. |

## Running

```sh
cmake --build build/cmake -j          # always rebuild before timing anything
python3 benchmarks/run_benchmarks.py  # all benches, -O0..-O3 + paired -O2
python3 benchmarks/run_benchmarks.py --only checks_on --runs 7
python3 benchmarks/run_benchmarks.py --c-musl  # Linux: C on the PL/I static musl
python3 benchmarks/run_tasking_bench.py --models fiber,os
```

Every measured binary is rebuilt on every run, so a compiler rebuild can never
be timed against a stale executable. `--force` is accepted and redundant.

The PL/I and C samples are interleaved (one PL/I run, one C run, repeated), so
a drifting CPU clock is shared by both sides instead of pricing whichever side
runs second. The result manifest records `"sampling": "interleaved"` and
`"c_runtime": "host"` (or `"musl-static"`); the regression guard only compares
runs whose sampling and C runtime match.

On Linux the PL/I driver links static musl while the host C compiler defaults
to glibc. `--c-musl` (Linux only) builds the C twin with clang's
`--target=<arch>-linux-musl -static`, so the ratio prices code generation
rather than the libc. It requires the musl target headers and `libc.a` (e.g.
Ubuntu's `musl-tools`) and is opt-in: the default host-libc table is unchanged.

## What makes a number valid

The harness's only job is to turn runs into numbers, and it refuses to do so
when the run is not trustworthy:

- A warmup and every timed run must exit 0 and print the **same** output.
  A crash, a timeout or an unstable workload is reported as a status
  (`run_failed`, `run_timeout`, `warmup_failed`, `output_mismatch`), never
  folded into a median.
- A PL/I/C row is published only when the outputs match (`✓` exact, `≈`
  float-tolerant). `decimal_ops` is marked `~` descriptive: its PL/I side is
  fixed decimal and its C side is `double`, so the row informs but cannot gate.
- The `ir` geomean is reported only when **every** gate row is valid. A failed
  or missing row prints `NOT EVALUATED`, and `--only` runs are measurements,
  not the corpus gate.
- Paired comparisons must print identical output on both sides; a failed pair
  fails the run.
- Any failed row or pair makes the process exit non-zero.

## Result files

`results/benchmark_results_<run id>.json` (plus `summary_<run id>.csv`) holds
the rows, the raw samples, machine and toolchain identity, the effective
pinning mode, and a **manifest** of the settings: benchmark set, flags, runs,
platform, source hashes and the compiler binary hash.

The regression guard compares `-O2` ratios only against a run whose manifest
matches, and prints how many valid rows it compared. A compiler change is
noted but still compared — that is the case the guard exists for. Use
`--compare-to FILE` to choose a baseline deliberately.

## Timing notes

Pinning is probed once and its outcome recorded: `taskset` on Linux (first CPU
allowed by the scheduling affinity), `nice -n -20` on macOS when the host
permits it. macOS `nice` exits 0 even when it is refused, so exit status alone
is not treated as evidence. Rows whose PL/I median is under 50 ms are marked
`◦` — their ratios are indicative, not stable.

The C twin is built for the same target policy as the PL/I output: the driver
targets LLVM's generic CPU and does not contract FP operations, so C gets
`-march=x86-64` on x86_64, `-mcpu=generic` on arm64, and `-ffp-contract=off`.
Without that the C baseline would get the host's native tuning (for example
apple-m1) and fused multiply-adds, and the ratio would price the target policy
rather than code generation. `--c-native` builds C for the host CPU instead (the
FP-contraction policy stays matched); the effective flags are recorded in the
manifest (`c_flags`).

Tasking timings are whole-program wall times that include process startup;
`ns/task` is that time divided by 20 000 and inherits the startup cost.
