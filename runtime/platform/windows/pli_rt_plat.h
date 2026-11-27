/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* runtime/platform/windows/pli_rt_plat.h — Windows backend types. */
#ifndef PLI_RT_PLAT_WINDOWS_H
#define PLI_RT_PLAT_WINDOWS_H

/* Windows backend types (single selection block).
 * Keep the system header out of macro pollution: pli_rt.h defines
 * VOID/I64/... while expanding pli_rt_abi.def, and windows.h (winnt.h)
 * collapses with 'unknown type name CHAR/LONG/...' when VOID is a macro.
 * pli_rt.h includes pli_rt_state.h -> this header outside that macro
 * region, but guard here too so direct/odd-order inclusion stays safe. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
/* push_macro works even when the name is currently undefined (pop restores
 * that state) on MSVC/clang, so push/undef/pop unconditionally. */
#pragma push_macro("VOID")
#pragma push_macro("I64")
#pragma push_macro("I32")
#pragma push_macro("I8")
#pragma push_macro("DOUBLE")
#pragma push_macro("PTR")
#pragma push_macro("CPTR")
#pragma push_macro("IPTR")
#pragma push_macro("PLI_FN")
#undef VOID
#undef I64
#undef I32
#undef I8
#undef DOUBLE
#undef PTR
#undef CPTR
#undef IPTR
#undef PLI_FN
#include <windows.h>
#pragma pop_macro("PLI_FN")
#pragma pop_macro("IPTR")
#pragma pop_macro("CPTR")
#pragma pop_macro("PTR")
#pragma pop_macro("DOUBLE")
#pragma pop_macro("I8")
#pragma pop_macro("I32")
#pragma pop_macro("I64")
#pragma pop_macro("VOID")

#include <stdint.h>

typedef CRITICAL_SECTION pli_mutex;
typedef CONDITION_VARIABLE pli_cond;
typedef INIT_ONCE pli_once;
typedef uintptr_t pli_thread;
#define PLI_ONCE_INIT INIT_ONCE_STATIC_INIT

#endif /* PLI_RT_PLAT_WINDOWS_H */
