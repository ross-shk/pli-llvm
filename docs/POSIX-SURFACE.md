# POSIX dependencies used by `libpli`

This is a portability inventory, not a PL/I API reference. It groups the POSIX
library calls used by the C runtime so platform work can identify which parts
need wrappers or replacements.

| Area | POSIX interfaces | Runtime purpose |
|---|---|---|
| Files and streams | `fopen`, `fclose`, `fread`, `fwrite`, `getc`, `getchar`, `fputc`, `fflush`, `fprintf`, `stdout`, `stderr` | Stream and record I/O, diagnostics |
| Memory and bytes | `malloc`, `free`, `realloc`, `memcpy`, `memset`, `bzero` | Dynamic storage and data movement |
| Process | `exit` | Runtime termination |
| Time | `time`, `localtime_r` | Date and time built-ins |
| Threads and waiting | `nanosleep`, `errno`, pthread mutex/condition/thread functions | `TASK`/`EVENT` support and delays |

See [`CROSS-PLATFORM-PLAN.md`](CROSS-PLATFORM-PLAN.md) for the proposed MSVC
portability work.
