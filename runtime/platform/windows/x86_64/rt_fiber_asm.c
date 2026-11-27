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
    /* Windows x64 passes the arguments in RCX and RDX (not RDI/RSI), and
     * its callee-saved set is RBX RBP RDI RSI R12-R15 plus XMM6-XMM15. */
    "    movq    %rsp, %r8\n" /* S: the entry rsp; [S] holds the resume address */
    "    pushq   %rbp\n"       /* the register slots land at [S-64, S) */
    "    pushq   %rbx\n"
    "    pushq   %rdi\n"
    "    pushq   %rsi\n"
    "    pushq   %r12\n"
    "    pushq   %r13\n"
    "    pushq   %r14\n"
    "    pushq   %r15\n"
    "    subq    $160, %rsp\n" /* the XMM6-XMM15 save area, at [S-224, S-64) */
    "    movdqu  %xmm6, 0(%rsp)\n"
    "    movdqu  %xmm7, 16(%rsp)\n"
    "    movdqu  %xmm8, 32(%rsp)\n"
    "    movdqu  %xmm9, 48(%rsp)\n"
    "    movdqu  %xmm10, 64(%rsp)\n"
    "    movdqu  %xmm11, 80(%rsp)\n"
    "    movdqu  %xmm12, 96(%rsp)\n"
    "    movdqu  %xmm13, 112(%rsp)\n"
    "    movdqu  %xmm14, 128(%rsp)\n"
    "    movdqu  %xmm15, 144(%rsp)\n"
    "    movq    %r8, 0(%rcx)\n"  /* from->sp = S */
    "    movq    0(%rdx), %r11\n" /* to->sp = S' */
    "    movq    %r11, %rsp\n"
    "    movq    8(%rdx), %r11\n" /* to->pc */
    "    movdqu  -224(%rsp), %xmm6\n"
    "    movdqu  -208(%rsp), %xmm7\n"
    "    movdqu  -192(%rsp), %xmm8\n"
    "    movdqu  -176(%rsp), %xmm9\n"
    "    movdqu  -160(%rsp), %xmm10\n"
    "    movdqu  -144(%rsp), %xmm11\n"
    "    movdqu  -128(%rsp), %xmm12\n"
    "    movdqu  -112(%rsp), %xmm13\n"
    "    movdqu  -96(%rsp), %xmm14\n"
    "    movdqu  -80(%rsp), %xmm15\n"
    "    movq    -64(%rsp), %r15\n"
    "    movq    -56(%rsp), %r14\n"
    "    movq    -48(%rsp), %r13\n"
    "    movq    -40(%rsp), %r12\n"
    "    movq    -32(%rsp), %rsi\n"
    "    movq    -24(%rsp), %rdi\n"
    "    movq    -16(%rsp), %rbx\n"
    "    movq    -8(%rsp), %rbp\n"
    "    ret\n" /* pop [S'] = the resume address; rsp = S'+8 */
    ".globl " RT_FIBER_SYM("pli_fiber_start") "\n"
    RT_FIBER_SYM("pli_fiber_start") ":\n"
    /* Only the volatile registers may be touched: rsi and the other
     * callee-saved registers belong to the caller, and a hand-written
     * function that clobbers them corrupts the caller's live values (the
     * scheduler keeps &sched_ctx in rsi across this call). */
    "    movq    %rcx, %r10\n" /* r10 = f */
    "    movq    %r8, 8(%r10)\n" /* f->pc = the entry point */
    "    leaq    -64(%rdx), %rax\n"
    "    andq    $-16, %rax\n"    /* S: 16-aligned, so the entry runs with rsp = S+8 */
    "    leaq    -224(%rax), %r9\n" /* the register save area */
    "    xorq    %r11, %r11\n"
    "    movq    $28, %rcx\n"      /* 224 bytes of callee-saved slots to clear */
    "1:  movq    %r11, (%r9)\n"
    "    addq    $8, %r9\n"
    "    decq    %rcx\n"
    "    jnz     1b\n"
    "    movq    %rax, 0(%r10)\n" /* f->sp = S */
    "    movq    %r8, (%rax)\n"   /* [S] = the entry address */
    "    movq    $0, 8(%rax)\n"   /* [S+8] = the entry's return address */
    "    ret\n");
