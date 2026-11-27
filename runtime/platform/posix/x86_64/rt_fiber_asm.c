/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* rt_fiber_asm.c — the fiber context switch, as module assembly (ADR-234). */

#if defined(__APPLE__)
#define RT_FIBER_SYM(name) "_" name
#else
#define RT_FIBER_SYM(name) name
#endif

__asm__(
    ".text\n"
    ".globl " RT_FIBER_SYM("pli_fiber_switch") "\n"
    RT_FIBER_SYM("pli_fiber_switch") ":\n"
    "    movq    %rsp, %r8\n" /* S: the entry rsp; [S] holds the resume address */
    "    pushq   %rbp\n"       /* the register slots land at [S-48, S) */
    "    pushq   %rbx\n"
    "    pushq   %r12\n"
    "    pushq   %r13\n"
    "    pushq   %r14\n"
    "    pushq   %r15\n"
    "    movq    %r8, 0(%rdi)\n" /* from->sp = S */
    "    movq    0(%rsi), %rdx\n" /* to->sp = S' */
    "    movq    %rdx, %rsp\n"
    "    movq    -48(%rsp), %r15\n"
    "    movq    -40(%rsp), %r14\n"
    "    movq    -32(%rsp), %r13\n"
    "    movq    -24(%rsp), %r12\n"
    "    movq    -16(%rsp), %rbx\n"
    "    movq    -8(%rsp), %rbp\n"
    "    ret\n" /* pop [S'] = the resume address; rsp = S'+8 */
    ".globl " RT_FIBER_SYM("pli_fiber_start") "\n"
    RT_FIBER_SYM("pli_fiber_start") ":\n"
    "    movq    %rdx, 8(%rdi)\n" /* f->pc = the entry point */
    "    leaq    -64(%rsi), %rax\n"
    "    andq    $-16, %rax\n"    /* S: 16-aligned, so the entry runs with rsp = S+8 */
    "    leaq    -48(%rax), %rcx\n"
    "    xorq    %r8, %r8\n"
    "    movq    $6, %r9\n"
    "1:  movq    %r8, (%rcx)\n" /* the callee-saved registers start undefined */
    "    addq    $8, %rcx\n"
    "    decq    %r9\n"
    "    jnz     1b\n"
    "    movq    %rax, 0(%rdi)\n" /* f->sp = S */
    "    movq    %rdx, (%rax)\n"  /* [S] = the entry address: the switch's RET jumps there */
    "    movq    $0, 8(%rax)\n"  /* [S+8] = the entry's return address (never used) */
    "    ret\n");
