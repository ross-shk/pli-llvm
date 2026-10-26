# Tasking benchmarks

ADR-234's lightweight-fiber model versus the legacy ADR-016 OS-thread
model, each measured against Go's goroutines and C's raw OS threads.

## Models

| Env var | Value | Model | ADR |
|---|---|---|---|
| `PLI_TASK_MODEL` | `fiber` (default) | one OS thread per worker; tasks are fibers on a bounded pool | ADR-234/235/236/237 |
| `PLI_TASK_MODEL` | `os` | one OS thread per task ("multithreading") | ADR-016 |

`CALL ... TASK/EVENT` is the only source-level difference — the program text
does not change between models. The model is selected at runtime by
`PLI_TASK_MODEL`; the harness sets it per run.

## Workloads

Both workloads spawn 20 000 tasks that do nothing (N = 20 000 in
`run_tasking_bench.py`):

* **W1 — `task_spawn`** (fan-out / fan-in): start all N tasks, then wait for
  all N. Every task is live at once, so this measures the cost of
  *creating* the task's execution context (the stack) as much as the
  tasking itself.
* **W1b — `task_loop`** (task lifecycle): start a task, wait for it,
  N times. The fiber pool recycles the stack, so this measures the pure
  spawn – switch – join cycle with no live overlap.

Each workload has a `.pli` source (compiled with `pli`), a C baseline
(`*_c.c`, compiled with `clang -O2`), and a Go baseline (`*.go`).

## How to run

```sh
# both models, gate check on fiber only
python3 benchmarks/run_tasking_bench.py --models fiber,os

# a single model
python3 benchmarks/run_tasking_bench.py --models os

# report the numbers but do not fail on the fiber gate
python3 benchmarks/run_tasking_bench.py --models fiber,os --no-gate
```

Results are saved to `benchmarks/results/tasking_<run id>.json`.

## What a number means

The harness times the whole program (one warmup plus `--runs` timed runs,
median) and the tables divide that by 20 000 to get `ns/task`. Process
startup is inside every number, which is why the gate is startup-sensitive on
these short workloads. The C and Go baselines do not depend on the PL/I
model, so they are measured once per workload and shared by the `fiber` and
`os` rows. A run is admitted only if the process exited 0 and printed
`done`: a crashed or truncated workload would otherwise look fast. The C
references fail loudly when a thread cannot be created, so resource
exhaustion cannot be reported as a win.

## Baseline measurement (before ADR-313)

macOS 26.6.2 (arm64, 8 cores), pli v1.0.0-99-gaa83aa8 / LLVM 23.1.2,
Go 1.27.3, Apple clang 17.0.0, 20 000 tasks, 9 runs, median. The current
numbers are in the update below.

### ns/task

| Workload | PL/I (fibers) | PL/I (OS threads) | Go (goroutines) | C (OS threads) |
|---|---|---|---|---|
| **task_loop** (W1b) | 501.1 ns | 520.6 ns | 626.1 ns | 24 618.6 ns |
| **task_spawn** (W1)  | 13 365.6 ns | 14 078.9 ns | 617.1 ns | 73 940.3 ns |

### PL/I / Go ratio (median)

| Model | task_loop | task_spawn |
|---|---|---|
| **fiber** (ADR-234 default) | **0.800×** | 21.658× |
| **os** (ADR-016 multithreading) | **0.706×** | 20.903× |

**Fiber gate: PASS** (0.800× ≤ 0.90× on W1b).

### PL/I / C ratio (OS threads baseline — how much the fiber model buys)

| Model | task_loop | task_spawn |
|---|---|---|
| **fiber** | 0.020× | 0.181× |
| **os** | 0.020× | 0.173× |

The fiber model is ~50× faster than raw OS threads on task_loop and ~5.5× on
task_spawn — the stack and scheduler are shared across all tasks instead of
allocated per thread.

### Update: ADR-313, the POSIX fiber stack arena

The table above is the measurement before ADR-313, when each POSIX fiber
`mmap`ed its whole ceiling. Both binaries were then run alternately on the
same host (21 rounds interleaved, median, 20 000 tasks):

| task_spawn | ns/task | PL/I / Go |
|---|---|---|
| before (one `mmap` per fiber) | 13 364 | 24.8× |
| after (arena + chunked commit) | **6 997** | **13.0×** |
| Go | 539 | — |

The lifecycle is unaffected: it recycles stacks from the fiber pool, so it
never paid the reservation. The other columns above were not re-measured.

### Update: ADR-324 and ADR-325, the fan-out path

ADR-324 commits one page at allocation instead of the top 64 KiB chunk, and
ADR-325 wakes an idle peer while a fan-out is still spawning, so finished
tasks recycle their stacks instead of every task keeping one. Same host,
20 000 tasks, median of 9 or 15 runs:

| task_spawn | ns/task | PL/I / Go |
|---|---|---|
| after ADR-313 (top chunk, no wake) | 6 715 | 11.85× |
| ADR-324 (top page) | 5 859 | 9.30× |
| ADR-325, wake on every spawn | 3 075 | 5.57× |
| ADR-325, wake at most every 200 µs | **744–821** | **1.1–1.5×** |

Go read 10.7–13.8 ms across those runs, so the ratio moves with the Go side;
the PL/I absolute time (14.9–16.4 ms) is the stable figure. Peak RSS on a
200 000-task fan-out fell from 1.4 GiB to 20.9 MiB. The lifecycle is
unchanged (0.66–0.72× Go), and the gate still holds.

`benchmarks/tasking/stack_cost.c` is the microbenchmark behind the one-page
figure: it prices a PROT_NONE arena, the `mprotect`, and the first touch with
no scheduler in the way.

### Fiber context-switch floor

`benchmarks/tasking/fiber_floor.c` measures the raw start + join with no
scheduler, no locking and no allocation (the stack is allocated once and
reused):

| Platform | Floor | Go goroutine spawn + Done | Floor / Go |
|---|---|---|---|
| macOS arm64 | 42.0 ns | ~210 ns | 0.13× |
| Windows x64 (ADR-234) | 28.0 ns | ~210 ns | 0.13× |

## Notes

* **task_spawn is now close to Go.** The fiber stacks come from an arena of
  slots, start with one page committed and grow in 64 KiB chunks
  (ADR-313/324), so a ceiling costs address space and not a reservation.
  ADR-325 wakes a peer during the spawn loop, so a fan-out recycles the
  stacks of tasks that finish instead of keeping one for every task: a
  200 000-task fan-out peaks at ~21 MiB, not 1.4 GiB. A task still costs
  the one page it first touches — 16 KiB on Apple silicon — and Go's 2 KiB
  copied stack stays smaller, which is the remaining difference. The fiber
  floor (42 ns) proves the switch is not the problem.

* **task_loop beats Go on this platform** (fiber: 0.75×–0.80×, os: 0.57×–0.77×
  when unloaded). The lifecycle workload recycles stacks from the pool, so the
  reservation cost is paid once. OS thread creation (`pthread_create`) is fast
  on macOS, and the event-based wait avoids the futex wake-up Go's
  `WaitGroup` pays. On Windows x64 (ADR-234) the same measurement gave
  470.8 ns (fiber) vs 981.3 ns (Go) for the fiber model — the gap narrows at
  0.48× — and 2510.6 ns vs 995.9 ns for the OS-thread model, i.e. 2.52×
  Go, showing the Windows OS-thread path is the expensive one where Go wins
  by being cheaper than a raw thread.

* **The `os` model is highly variable under load.** Creating 20 000 OS
  threads taxes the kernel scheduler: on an unloaded macOS arm64 host the
  task_loop cost per task is ~470–520 ns (faster than Go), but under
  concurrent load it degrades to ~25 µs/task — matching C's raw
  `pthread_create`+`pthread_join` and 34× Go's time. The fiber model
  stays bounded at ~500 ns/task regardless of load, because only
  `min(cores, 8)` OS threads ever exist. This is the central reason
  ADR-234 replaced the OS-thread model with the fiber model: not raw
  throughput, but load independence.

* **The fiber gate is noisy on macOS** (~0.70×–1.58× across run sets).
  task_loop is a ~5–10 ms program where process startup is 2–3 ms, so the
  median swings 20–30 % run-to-run. The gate check reports when the median
  slips above 0.90×; this is a startup-sensitive diagnostic, not a codegen
  regression test. Larger N or an in-process timer loop would tighten it.
