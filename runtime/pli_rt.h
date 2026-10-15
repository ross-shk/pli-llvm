/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli: a PL/I compiler targeting LLVM */
/* pli_rt.h — PL/I runtime library (pli_runtime), M0 subset.
 *
 * Prototypes are generated from runtime/pli_rt_abi.def, the single source of
 * truth for the pli_* ABI (names are the ABI: see docs/ARCHITECTURE.md
 * "Runtime interface"). Each PLI_FN line becomes one prototype; the .def also
 * drives irgen's LLVM declarations, so the two cannot drift.
 */
#ifndef PLI_RT_H
#define PLI_RT_H

/* Before extern "C": the wide-int header carries C++ operator overloads on
 * MSVC (portable fallback), which cannot live in a C-linkage block. */

#include "pli_wide128.h"

/* Shared TU state + platform seam. Included BEFORE the .def type macros
 * below: pli_rt_state.h -> pli_rt_plat.h pulls in system headers
 * (<windows.h> on Win, <pthread.h> elsewhere), which must never be
 * preprocessed while VOID/I64/I32/I8/DOUBLE/PTR/CPTR/IPTR are macros
 * (windows.h breaks: winnt.h cascade 'unknown type name CHAR/LONG/...'). */

#include "pli_rt_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Expand the .def's type tokens to C types. Wide (>18 digit) DECIMAL values
 * cross the ABI as (lo, hi) I64 halves (see pli_rt_abi.def); the portable
 * 128-bit integer below is for arithmetic/formatting inside the runtime only
 * (MSVC lacks __int128; clang-cl supports it natively).
 * Scope is tightly limited to the pli_rt_abi.def inclusion below. */
#define VOID    void
#define I64     long long
#define I32     int
#define I8      unsigned char
#define DOUBLE  double
#define PTR     char *
#define CPTR    const char *
#define IPTR    int *
#define PLI_FN(name, ret, args) ret name args;

#include "pli_rt_abi.def"

#undef PLI_FN
#undef IPTR
#undef CPTR
#undef PTR
#undef DOUBLE
#undef I8
#undef I32
#undef I64
#undef VOID

#ifdef __cplusplus
}
#endif
#endif /* PLI_RT_H */
