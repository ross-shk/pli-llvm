/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* rt_fiber.S — the fiber context switch (ADR-234).
 *
 * Two entry points:
 *   void pli_fiber_switch(pli_fiber *from, pli_fiber *to);
 *   void pli_fiber_start(pli_fiber *f, void *stack_top, void *entry);
 *
 * The switch saves the callee-saved registers of `from` and jumps to `to`,
 * which resumes exactly where it left off. The pair is symmetric: switching
 * back to `from` resumes it after the switch that suspended it.
 *
 * The saved set is the platform ABI's callee-saved set, because a C
 * function suspended inside this call keeps its live values there:
 *   x86-64 SysV  — RBX RBP R12-R15
 *   x86-64 Win64 — RBX RBP RDI R12-R15 and XMM6-XMM15
 * (the AArch64 variant lands in a later phase, with its CI coverage).
 *
 * The saved stack pointer is the switch's ENTRY stack pointer, and the
 * word at that address holds the resume address: it is the return address
 * of the pli_fiber_switch call in the suspended code. The restore ends
 * with a RET, which pops that word and leaves the resumed code with
 * exactly the stack pointer it had at the call site. Saving the
 * post-push stack pointer instead would leave the resumed frame eight
 * bytes off, and jumping to the saved address instead of returning
 * would leave the return address on the stack.
 *
 * The pli_fiber layout the assembly relies on (see runtime/rt_fiber.h):
 *   offset 0 — the saved stack pointer
 */
#if defined(__x86_64__) && !defined(_WIN32)

    .text
    .globl pli_fiber_switch
#if !defined(_WIN32)
    .type pli_fiber_switch,@function
#endif
pli_fiber_switch:
    movq    %rsp, %r8           /* S: the entry rsp; [S] holds the resume address */
    pushq   %rbp                /* the register slots land at [S-48, S) */
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    movq    %r8, 0(%rdi)        /* from->sp = S */
    movq    0(%rsi), %rdx       /* to->sp = S' */
    movq    %rdx, %rsp
    movq    -48(%rsp), %r15
    movq    -40(%rsp), %r14
    movq    -32(%rsp), %r13
    movq    -24(%rsp), %r12
    movq    -16(%rsp), %rbx
    movq    -8(%rsp), %rbp
    ret                         /* pop [S'] = the resume address; rsp = S'+8 */
#if !defined(_WIN32)
    .size pli_fiber_switch,.-pli_fiber_switch
#endif

    .globl pli_fiber_start
#if !defined(_WIN32)
    .type pli_fiber_start,@function
#endif
pli_fiber_start:
    /* rdi = f, rsi = stack_top, rdx = entry.
     * Lay up a fresh fiber: S must be 16-aligned, because the entry point
     * runs with rsp = S+8 (8 mod 16, the ABI's entry alignment). Leave 64
     * bytes of headroom below the stack top so the entry's shadow space
     * fits, then zero the register slots. */
    movq    %rdx, 8(%rdi)        /* f->pc = the entry point */
    leaq    -64(%rsi), %rax
    andq    $-16, %rax
    leaq    -48(%rax), %rcx
    xorq    %r8, %r8
    movq    $6, %r9
1:  movq    %r8, (%rcx)          /* the callee-saved registers start undefined */
    addq    $8, %rcx
    decq    %r9
    jnz     1b
    movq    %rax, 0(%rdi)        /* f->sp = S */
    movq    %rdx, (%rax)         /* [S] = the entry address: the switch's RET jumps there */
    movq    $0, 8(%rax)          /* [S+8] = the entry's return address (never used) */
    ret
#if !defined(_WIN32)
    .size pli_fiber_start,.-pli_fiber_start
#endif

#elif defined(__x86_64__) && defined(_WIN32)

    .text
    .globl pli_fiber_switch
#if !defined(_WIN32)
    .type pli_fiber_switch,@function
#endif
pli_fiber_switch:
    /* Windows x64 passes the arguments in RCX and RDX (not RDI/RSI). */
    movq    %rsp, %r8           /* S: the entry rsp; [S] holds the resume address */
    pushq   %rbp                /* the register slots land at [S-56, S) */
    pushq   %rbx
    pushq   %rdi
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    subq    $160, %rsp          /* the XMM6-XMM15 save area, at [S-216, S-56) */
    movdqu  %xmm6, 0(%rsp)
    movdqu  %xmm7, 16(%rsp)
    movdqu  %xmm8, 32(%rsp)
    movdqu  %xmm9, 48(%rsp)
    movdqu  %xmm10, 64(%rsp)
    movdqu  %xmm11, 80(%rsp)
    movdqu  %xmm12, 96(%rsp)
    movdqu  %xmm13, 112(%rsp)
    movdqu  %xmm14, 128(%rsp)
    movdqu  %xmm15, 144(%rsp)
    movq    %r8, 0(%rcx)        /* from->sp = S */
    movq    0(%rdx), %r11       /* to->sp = S' */
    movq    %r11, %rsp
    movdqu  -216(%rsp), %xmm6
    movdqu  -200(%rsp), %xmm7
    movdqu  -184(%rsp), %xmm8
    movdqu  -168(%rsp), %xmm9
    movdqu  -152(%rsp), %xmm10
    movdqu  -136(%rsp), %xmm11
    movdqu  -120(%rsp), %xmm12
    movdqu  -104(%rsp), %xmm13
    movdqu  -88(%rsp), %xmm14
    movdqu  -72(%rsp), %xmm15
    movq    -56(%rsp), %r15
    movq    -48(%rsp), %r14
    movq    -40(%rsp), %r13
    movq    -32(%rsp), %r12
    movq    -24(%rsp), %rdi
    movq    -16(%rsp), %rbx
    movq    -8(%rsp), %rbp
    ret                         /* pop [S'] = the resume address; rsp = S'+8 */
#if !defined(_WIN32)
    .size pli_fiber_switch,.-pli_fiber_switch
#endif

    .globl pli_fiber_start
#if !defined(_WIN32)
    .type pli_fiber_start,@function
#endif
pli_fiber_start:
    /* rcx = f, rdx = stack_top, r8 = entry. The save area is the XMM
     * block (160 bytes) plus the seven register slots (56 bytes). */
    movq    %r8, 8(%rcx)         /* f->pc = the entry point */
    leaq    -64(%rdx), %rax
    andq    $-16, %rax
    leaq    -216(%rax), %r10
    movq    %r10, %r9
    xorq    %r11, %r11
    movq    $27, %rsi
1:  movq    %r11, (%r9)          /* the callee-saved registers start undefined */
    addq    $8, %r9
    decq    %rsi
    jnz     1b
    movq    %rax, 0(%rcx)        /* f->sp = S */
    movq    %r8, (%rax)          /* [S] = the entry address: the switch's RET jumps there */
    movq    $0, 8(%rax)          /* [S+8] = the entry's return address (never used) */
    ret
#if !defined(_WIN32)
    .size pli_fiber_start,.-pli_fiber_start
#endif

#endif /* __x86_64__ */
