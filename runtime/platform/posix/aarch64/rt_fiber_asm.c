/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* rt_fiber_asm.c — the fiber context switch, as module assembly (ADR-234). */

#if defined(__APPLE__)
#define RT_FIBER_SYM(name) "_" name
#else
#define RT_FIBER_SYM(name) name
#endif

/* The AArch64 variant.
 *
 * The contract is the same as on x86-64, but the ABI keeps the return
 * address in x30 rather than on the stack, so the shape of the frame
 * differs. The frame is 176 bytes at [S-176, S), where S is the switch's
 * entry stack pointer (always 16-byte aligned, as the ABI requires):
 *
 *     [S-176, S-112)  d8-d15   the low 64 bits of the SIMD callee-saved set
 *     [S-112, S-32)   x19-x28  the general callee-saved registers
 *     [S-32, S-16)    x29, x30 the frame pointer and the link register
 *     [S-16, S)       padding, so the frame stays 16-byte aligned
 *
 * x30 is the resume address: it holds the return address of the
 * pli_fiber_switch call in the suspended code, exactly as x86-64 keeps it
 * in the word at [S]. So the restore ends with `ret`, after moving the
 * stack pointer back to S: the resumed code then sees the stack pointer
 * it had at the call site. (A `ret` with sp still at the frame base would
 * hand the caller a stack pointer 176 bytes too low.)
 *
 * Unlike x86-64, the saved fiber pointer is the FRAME BASE (S-176), not
 * S: the restore loads the slots at positive offsets from it and then
 * adds 176 to recover S. */
__asm__(
    ".text\n"
    ".globl " RT_FIBER_SYM("pli_fiber_switch") "\n"
    RT_FIBER_SYM("pli_fiber_switch") ":\n"
    "    mov     x9, sp\n"           /* S: the entry sp */
    "    sub     sp, sp, #176\n"     /* the register slots land at [S-176, S) */
    "    stp     d8,  d9,  [sp, #0]\n"
    "    stp     d10, d11, [sp, #16]\n"
    "    stp     d12, d13, [sp, #32]\n"
    "    stp     d14, d15, [sp, #48]\n"
    "    stp     x19, x20, [sp, #64]\n"
    "    stp     x21, x22, [sp, #80]\n"
    "    stp     x23, x24, [sp, #96]\n"
    "    stp     x25, x26, [sp, #112]\n"
    "    stp     x27, x28, [sp, #128]\n"
    "    stp     x29, x30, [sp, #144]\n" /* x30 is the resume address */
    "    sub     x11, x9, #176\n"     /* the frame base = S-176 */
    "    str     x11, [x0]\n"         /* from->sp = the frame base */
    "    ldr     x10, [x1]\n"        /* to->sp = S' */
    "    mov     sp, x10\n"
    "    ldp     d8,  d9,  [sp, #0]\n"
    "    ldp     d10, d11, [sp, #16]\n"
    "    ldp     d12, d13, [sp, #32]\n"
    "    ldp     d14, d15, [sp, #48]\n"
    "    ldp     x19, x20, [sp, #64]\n"
    "    ldp     x21, x22, [sp, #80]\n"
    "    ldp     x23, x24, [sp, #96]\n"
    "    ldp     x25, x26, [sp, #112]\n"
    "    ldp     x27, x28, [sp, #128]\n"
    "    ldp     x29, x30, [sp, #144]\n"
    "    add     sp, sp, #176\n"     /* sp = S', as the resumed code expects */
    "    ret\n"                      /* jump to the restored x30 */
    ".globl " RT_FIBER_SYM("pli_fiber_start") "\n"
    RT_FIBER_SYM("pli_fiber_start") ":\n"
    "    mov     x9, x1\n"           /* the stack's top */
    "    sub     x10, x9, #16\n"
    "    and     x10, x10, #-16\n"   /* S: 16-aligned */
    "    mov     x11, x10\n"
    "    sub     x11, x11, #176\n"   /* the register save area */
    "    mov     x12, #22\n"         /* 176 bytes of callee-saved slots to clear */
    "    mov     x13, xzr\n"
    "1:  str     x13, [x11], #8\n"
    "    sub     x12, x12, #1\n"
    "    cbnz    x12, 1b\n"          /* x11 is its base + 176 = S again */
    "    sub     x11, x11, #176\n"
    "    str     x2, [x11, #152]\n"  /* x30 = the entry point: the restore's ret */
    "    str     x11, [x0]\n"        /* f->sp = the frame base */
    "    str     x2, [x0, #8]\n"     /* f->pc = the entry point */
    "    ret\n");
