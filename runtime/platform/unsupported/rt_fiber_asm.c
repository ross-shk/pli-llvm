/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* rt_fiber_asm.c — the fiber context switch, as module assembly (ADR-234). */

#if defined(__APPLE__)
#define RT_FIBER_SYM(name) "_" name
#else
#define RT_FIBER_SYM(name) name
#endif

/* A platform without a fiber backend: the runtime keeps the legacy
 * OS-thread model, and the switch reports it if it is reached. */
__asm__(
    ".text\n"
    ".globl " RT_FIBER_SYM("pli_fiber_switch") "\n"
    RT_FIBER_SYM("pli_fiber_switch") ":\n"
    "    jmp     rt_fiber_unsupported\n"
    ".globl " RT_FIBER_SYM("pli_fiber_start") "\n"
    RT_FIBER_SYM("pli_fiber_start") ":\n"
    "    jmp     rt_fiber_unsupported\n");
