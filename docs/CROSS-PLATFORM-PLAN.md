# Cross-Platform Build Plan (MSVC Windows + existing Linux/macOS)

**Target:** compile `plic` (`plic.exe` on Windows) and `libpli` under the
native Visual Studio 2022+ toolchain, alongside the existing gcc/clang
Makefile builds on Linux/macOS. Pure MSVC — no MinGW, Cygwin, WSL, or
cross-compilation shims. CMake is the Windows build; the Makefile stays
POSIX-only.

**Status:** proposed work, not an implemented build path. The code samples in
this document show the intended changes; they are not APIs already present in
the repository. Recheck each inventory item against current source before
implementing it.

---

## Inventory (all verified against source)

| # | Item | Location | Unix dependency |
|---|------|----------|-----------------|
| A | `rt_task.c` calls `pthread_*` directly | `runtime/rt_task.c` passim | Stock MSVC ships no libpthread |
| B | Unconditional `#include <unistd.h>` | `src/main.cpp:12` | POSIX-only header |
| C | `popen()`/`pclose()` | `src/main.cpp:77-86` | MSVC names are `_popen`/`_pclose` |
| D | `shellQuote()` emits Bourne `'..'` quoting | `src/main.cpp:89-94` | `cmd.exe` needs `".."` quoting |
| E | PATH split on hardcoded `':'` | `src/main.cpp:105, 245` | Windows uses `';'` |
| F | `getpid()` unguarded | `src/main.cpp:327` | Declared in `unistd.h` only |
| G | `-pthread` emitted unconditionally | `src/main.cpp:360-362` | Unknown flag for MSVC/`lld-link` |
| H | `2>/dev/null` redirection in `runCapture` arg | `src/main.cpp:302` | `cmd.exe` needs `2>NUL` |
| 1 | `CMakeLists.txt:51` references nonexistent `runtime/pli_rt.c` | `CMakeLists.txt:51` | Bug — CMake build completely broken |
| 2 | Test runner uses `os.killpg`/`os.getpgid` + `start_new_session=True` | `tests/run_tests.py:47-53` | Neither exists on Windows Python |
| 3 | Test runner shells out to external `diff -u` | `tests/run_tests.py:105` | No GNU diff on stock Windows |
| 4 | Makefile LLVM discovery checks Apple-Silicon Homebrew first, generic `which` second | `Makefile:8-11` | Misses Intel-Mac Homebrew (`/usr/local/opt/llvm`), versioned `llvm-config-*` |

Sections 2C–4 of the previous revision of this file were corrupted
(mojibake/whitespace damage). They are rewritten below; sections 1–2B are
kept with technical corrections noted inline.

---

## 1. Runtime threading abstraction (mandatory on MSVC)

Stock LLVM-for-Windows binaries are MSVC-built, not linked against
winpthread. Every `pthread_*` call in `rt_task.c` (the only runtime file
using threads) moves behind a wrapper. All other `rt_*.c` files use plain
C stdlib and are portable as-is.

### New file: `runtime/sync/plic_thread.h`

```c
/* plic_thread.h — mutex/condvar/thread/sleep abstraction.
 * POSIX path wraps pthread.h; MSVC path wraps Win32
 * CriticalSection/ConditionVariable. Signatures identical. */
#ifndef PLIC_THREAD_H
#define PLIC_THREAD_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _MSC_VER

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>

typedef CRITICAL_SECTION   pli_mutex_t;
typedef CONDITION_VARIABLE pli_cond_t;

static inline void pli_mutex_init(pli_mutex_t *m) { InitializeCriticalSection(m); }
static inline void pli_mutex_lock(pli_mutex_t *m) { EnterCriticalSection(m); }
static inline void pli_mutex_unlock(pli_mutex_t *m) { LeaveCriticalSection(m); }
static inline void pli_cond_wait(pli_cond_t *cv, pli_mutex_t *m)
{ SleepConditionVariableCS(cv, m, INFINITE); }
static inline void pli_cond_broadcast(pli_cond_t *cv) { WakeAllConditionVariable(cv); }

/* _beginthreadex needs a __stdcall entry; PL/I task bodies are cdecl.
 * Do NOT pun the function pointer via union (strict-aliasing UB, breaks
 * x86-32 callee/caller cleanup). Box fn+arg and bounce through a shim. */
struct pli_spawn_box { void *(*fn)(void *); void *arg; };
static unsigned __stdcall pli_spawn_shim(void *p) {
  struct pli_spawn_box *b = (struct pli_spawn_box *)p;
  void *(*fn)(void *) = b->fn; void *arg = b->arg;
  free(b);
  fn(arg);
  return 0;
}

static inline void pli_spawn(void *(*fn)(void *), void *arg) {
  struct pli_spawn_box *b = (struct pli_spawn_box *)malloc(sizeof *b);
  if (!b) { fprintf(stderr, "TASK: out of memory\n"); exit(8); }
  b->fn = fn; b->arg = arg;
  if (!_beginthreadex(NULL, 0, pli_spawn_shim, b, 0, NULL)) {
    fprintf(stderr, "TASK: failed to create thread\n"); exit(8);
  }
}

static inline void pli_sleep_ms(long long ms) { Sleep((DWORD)ms); }

#else /* POSIX (gcc/clang, Linux/macOS) */

#include <errno.h>
#include <pthread.h>
#include <time.h>

typedef pthread_mutex_t pli_mutex_t;
typedef pthread_cond_t  pli_cond_t;

static inline void pli_mutex_init(pli_mutex_t *m) { pthread_mutex_init(m, NULL); }
static inline void pli_mutex_lock(pli_mutex_t *m) { pthread_mutex_lock(m); }
static inline void pli_mutex_unlock(pli_mutex_t *m) { pthread_mutex_unlock(m); }
static inline void pli_cond_wait(pli_cond_t *cv, pli_mutex_t *m)
{ pthread_cond_wait(cv, m); }
/* CORRECTION vs previous revision: takes a pointer, like the MSVC branch. */
static inline void pli_cond_broadcast(pli_cond_t *cv) { pthread_cond_broadcast(cv); }

static inline void pli_spawn(void *(*fn)(void *), void *arg) {
  pthread_t th;
  if (pthread_create(&th, NULL, fn, arg) != 0) {
    fprintf(stderr, "TASK: failed to create thread\n"); exit(8);
  }
  pthread_detach(th);
}

static inline void pli_sleep_ms(long long ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000L;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) ;
}

#endif
#endif /* PLIC_THREAD_H */
```

Corrections vs the previous revision: `pli_cond_broadcast` now takes a
pointer on both branches; the `cdecl`/`stdcall` union pun is replaced by a
heap-boxed `__stdcall` shim; `pli_sleep_ms` takes `long long`.

### Changes to `runtime/rt_task.c` (~35 lines, zero ABI change)

1. Includes: drop `<errno.h>`, `<pthread.h>`, `<time.h>`; add
   `"sync/plic_thread.h"`. (There was never a `<pty.h>`/`<unistd.h>`
   include here — the previous revision's note to that effect was wrong.)
2. Globals: replace the three `PTHREAD_*_INITIALIZER` literals with
   plain `static pli_mutex_t` / `static pli_cond_t` storage plus one-time
   init. Win32 `CONDITION_VARIABLE` has no static initializer, so lazy
   init is required — but it must be thread-safe. The previous revision's
   `volatile int` flag is racy; use `pthread_once` / `InitOnceExecuteOnce`:
   ```c
   static pli_mutex_t pli_ev_mu, pli_task_mu;
   static pli_cond_t  pli_ev_cv;
   #ifdef _MSC_VER
   static INIT_ONCE pli_sync_once_ctl = INIT_ONCE_STATIC_INIT;
   static BOOL CALLBACK pli_sync_init(PINIT_ONCE, PVOID, PVOID *) {
     pli_mutex_init(&pli_ev_mu); pli_mutex_init(&pli_task_mu);
     InitializeConditionVariable(&pli_ev_cv); return TRUE;
   }
   #else
   static pthread_once_t pli_sync_once_ctl = PTHREAD_ONCE_INIT;
   static void pli_sync_init(void) {
     pli_mutex_init(&pli_ev_mu); pli_mutex_init(&pli_task_mu);
     pthread_cond_init(&pli_ev_cv, NULL);
   }
   #endif
   static inline void pli_sync_once(void) {
   #ifdef _MSC_VER
     InitOnceExecuteOnce(&pli_sync_once_ctl, pli_sync_init, NULL, NULL);
   #else
     pthread_once(&pli_sync_once_ctl, pli_sync_init);
   #endif
   }
   ```
3. Call `pli_sync_once()` first in every entry point that touches the
   globals: `pli_event_reset/complete/wait`, `pli_wait_n`,
   `pli_event_status`, `pli_task_note`. (`pli_delay` needs no guard;
   `pli_task_spawn` touches no globals — guard harmless, omit it.)
4. Body renames, mechanically: `pthread_mutex_lock/unlock` →
   `pli_mutex_lock/unlock`; `pthread_cond_wait/broadcast(&pli_ev_cv, …)` →
   `pli_cond_wait/broadcast(&pli_ev_cv, …)`; `pthread_create`+`detach` →
   `pli_spawn(fn, ctx)`; `nanosleep` loop → `pli_sleep_ms(ms)`.
5. Include path: Makefile `RTCFLAGS += -I$(CURDIR)/runtime
   -I$(CURDIR)/runtime/sync`; CMake
   `target_include_directories(libpli PRIVATE runtime runtime/sync)`.

---

## 2. Compiler driver (`src/main.cpp`, ~25 net lines)

- **2A — pid:** delete `#include <unistd.h>` (line 12). Add:
  ```cpp
  #ifdef _WIN32
  #include <process.h>  // _getpid
  static int pli_get_pid() { return _getpid(); }
  #else
  #include <unistd.h>
  #include <sys/types.h>
  static pid_t pli_get_pid() { return getpid(); }
  #endif
  ```
  Line 327 becomes `std::to_string(pli_get_pid())`.
- **2B — popen:** before `runCapture`, add:
  ```cpp
  #ifdef _WIN32
  #define popen _popen
  #define pclose _pclose
  #endif
  ```
  (`_popen` is deprecated-but-present; add
  `#pragma warning(disable:4996)` if `/WX` is used. No other change —
  signatures match.)
- **2C — quoting:** keep the single `shellQuote()` name so all five call
  sites stay untouched; dispatch inside. The previous revision's "replace
  ALL with `shellQuoteCmd`" was wrong — POSIX `system()` runs `/bin/sh`
  (needs Bourne quoting), Windows `system()` runs `cmd.exe` (needs
  double-quote quoting):
  ```cpp
  static std::string shellQuote(const std::string &s) {
  #ifdef _WIN32
    std::string out = "\"";  // cmd.exe: wrap in ", escape " as \"
    for (char c : s) out += (c == '"' ? "\\\"" : std::string(1, c));
    return out + "\"";
  #else
    std::string out = "'";   // Bourne: wrap in ', escape ' as '\''
    for (char c : s) out += (c == '\'' ? "'\\''" : std::string(1, c));
    return out + "'";
  #endif
  }
  ```
  Covers the clang path, the temp `.ll`, the runtime lib, and the output
  (lines ~349–368) with zero call-site churn.
- **2D — PATH separator:** one shared separator for both `PATH` (line
  105) and `PLIC_INCLUDE_PATH` (line 245):
  ```cpp
  #ifdef _WIN32
  constexpr char pli_env_sep = ';';
  #else
  constexpr char pli_env_sep = ':';
  #endif
  ```
  then `std::getline(dirs, dir, pli_env_sep)` in both loops.
- **2E — stderr redirect:** line 302 appends `" 2>/dev/null"`; make it
  platform-dependent:
  ```cpp
  #ifdef _WIN32
  triple = runCapture((shellQuote(clangPath) + " -dumpmachine 2>NUL").c_str());
  #else
  triple = runCapture((shellQuote(clangPath) + " -dumpmachine 2>/dev/null").c_str());
  #endif
  ```
- **2F — link flags:** `-pthread` and `-Wl,--gc-sections` are meaningless
  to `lld-link` and must be skipped on Windows; `-Wl,-dead_strip` stays
  macOS-only:
  ```cpp
  #ifdef __APPLE__
    cmd += " -Wl,-dead_strip";
  #elif !defined(_WIN32)
    cmd += " -Wl,--gc-sections";
  #endif
  #ifndef _WIN32
    // No-op on macOS (libc carries pthreads); required on Linux/gcc.
    cmd += " -pthread";
  #endif
  ```
- **2G — release flags:** lines 342–344 pass Mach-O-only
  `-Wl,-dead_strip -Wl,-S`. Conditionalize:
  ```cpp
  if (release) {
    optLevel = "-O3";
  #ifdef __APPLE__
    backendFlags = " -Wl,-dead_strip -Wl,-S";
  #else
    backendFlags = "";  // size-trim via a post-build step if needed
  #endif
  }
  ```

---

## 3. Build-system fixes

### 3.1 `CMakeLists.txt` — fix critical bug (existing CMake build broken)

Line 51 names `runtime/pli_rt.c`, which does not exist. List exactly what
the Makefile compiles (11 files — the previous revision omitted
`rt_mathport.c`):

```cmake
set(RUNTIME_SRCS
  runtime/rt_core.c runtime/rt_stream.c runtime/rt_string.c
  runtime/rt_math.c runtime/rt_mathport.c runtime/rt_cond.c
  runtime/rt_storage.c runtime/rt_get.c runtime/rt_file.c
  runtime/rt_edit.c runtime/rt_task.c)
add_library(libpli STATIC ${RUNTIME_SRCS})
set_target_properties(libpli PROPERTIES OUTPUT_NAME pli)  # libpli.a / pli.lib
target_include_directories(libpli PRIVATE runtime runtime/sync)
target_compile_options(libpli PRIVATE $<$<C_COMPILER_ID:MSVC>:/utf-8>)
```

No extra thread link libs on MSVC (Win32 sync lives in `kernel32`).
`plic` target linkage and `PLIC_*` definitions are otherwise correct.

### 3.2 `Makefile` — wider LLVM discovery + sync include

Replace lines 8–11 with a chain covering Intel Macs and versioned binaries:

```make
LLVM_CONFIG ?= $(shell PATH="/opt/homebrew/opt/llvm/bin:/usr/local/opt/llvm/bin:$$PATH" \
  sh -c 'for c in llvm-config llvm-config-20 llvm-config-19 llvm-config-18; do \
    command -v $$c >/dev/null && { command -v $$c; break; }; done' 2>/dev/null)
```

and extend line 57:

```make
RTCFLAGS := $(CFLAGS) -ffunction-sections -fdata-sections \
  -I$(CURDIR)/runtime -I$(CURDIR)/runtime/sync
```

(The Makefile itself stays POSIX-only; Windows builds go through CMake.)

### 3.3 `tests/run_tests.py` — two portability fixes

- **Fix A — process kill:** replace the `os.killpg(os.getpgid(...))` block
  with `proc.kill()` + `proc.wait()`. `killpg`/`getpgid` do not exist on
  Windows, and `start_new_session=True` is POSIX-only, so guard it:
  ```python
  pops = {"stdout": fh, "stderr": subprocess.STDOUT}
  if os.name != "nt":
      pops["start_new_session"] = True
  proc = subprocess.Popen(cmd, **pops)
  try:
      proc.wait(timeout=timeout)
  except subprocess.TimeoutExpired:
      try:
          proc.kill()
      except OSError:
          pass
      proc.wait()
      return 124, True
  return proc.returncode, False
  ```
- **Fix B — diff:** drop the external `diff -u` dependency; compare in
  memory with `difflib.unified_diff` (output is near-identical to GNU
  `diff -u`) and return `FAIL` with the unified diff on mismatch.

### 3.4 CMake test integration

Existing `add_test` wiring (`PLIC`/`RTLIB`/`CLANG`) is already correct;
only add `PYTHONIOENCODING=utf-8` so test output decoding is stable on
Windows consoles:

```cmake
set_tests_properties(plic-tests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  ENVIRONMENT "PLIC=$<TARGET_FILE:plic>;RTLIB=$<TARGET_FILE:libpli>;CLANG=${LLVM_TOOLS_BINARY_DIR}/clang;PYTHONIOENCODING=utf-8")
```

### 3.5 Unified build commands (CMake is canonical)

Stock Windows ships no GNU `make` (VS `nmake` uses an incompatible syntax),
so literal `make -j8 && make test` cannot be unified. The portable spelling
is the CMake CLI — identical on Windows (Developer Command Prompt), macOS,
and Linux:

```sh
cmake -B build -DCMAKE_PREFIX_PATH=<llvm-root>  # once; C:\path\to\LLVM on Win
cmake --build build --config Release --parallel 8
ctest --test-dir build --parallel 8
cmake --install build --prefix <dest>           # was: make install
```

Prerequisites (all inside this plan): fix `CMakeLists.txt` per §3.1 and add
the missing `src/preprocessor.cpp` to `PLIC_SOURCES` (the Makefile builds
it; CMake omits it, so CMake currently neither configures nor links).
Then demote the `Makefile` to a thin POSIX wrapper — `all/test/install/clean`
forward to the four commands above — so `make -j8 && make test` keeps working
as an alias. `make check` splits: portable `check` (warnings + format, as
CMake custom targets) everywhere, POSIX-only `check-full` (adds `tidy` +
`scan-build`, which have no MSVC equivalent).

| make (POSIX alias) | canonical (all platforms) |
|---|---|
| `make -j8` | `cmake --build build --parallel 8` |
| `make test` | `ctest --test-dir build` |
| `make install` | `cmake --install build` |
| `make check` | portable subset only; full gate stays `make check-full` on POSIX |

### 3.6 Affected files

| File | Action | Net lines | Purpose |
|------|--------|-----------|---------|
| `runtime/sync/plic_thread.h` | NEW | ~70 | Thread/mutex/cond/sleep abstraction |
| `runtime/rt_task.c` | EDIT | ~35 | Use wrappers, thread-safe once-init |
| `src/main.cpp` | EDIT | ~25 | pid, popen, quoting, separators, flags |
| `CMakeLists.txt` | EDIT | ~15 | Real runtime list, missing source, sync include, `/utf-8`, portable `check` targets |
| `Makefile` | EDIT | ~10 | LLVM discovery, sync include, thin wrapper forwarding to CMake |
| `tests/run_tests.py` | EDIT | ~15 | Portable kill, `difflib` diff |
| **Total** | | **~170** | 1 new file, 5 edits |

---

## 4. Documentation updates

- `README.md`: document the §3.5 canonical commands as the primary build
  (requires VS 2022 with "Desktop development with C++", LLVM ≥ 18,
  CMake ≥ 3.20, Python 3; Windows runs them from a Developer Command
  Prompt with `-DCMAKE_PREFIX_PATH=C:\path\to\LLVM`, POSIX uses the
  LLVM prefix or `llvm-config` location). Keep `make -j8 && make test`
  documented as a POSIX alias.

---

## 5. Scope boundaries

Out of scope: compiler stages (lexer/parser/sema/irgen), builtin
implementations, grammar-coverage expansion, test-suite additions, `.vcxproj`
generation (CMake handles it), third-party DLL dependencies. Emitted LLVM IR
stays portable; the only MSVC-specific codegen note is `/MT` vs `/MD`
runtime selection, left to CMake defaults.
