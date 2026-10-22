/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* rt_fiber_asm.c — the fiber context switch, as module assembly (ADR-234).
 *
 * The switch is assembly, but it has to travel inside the runtime's
 * bitcode blob: the driver links the runtime in-process, so there is no
 * separate object file to link in. A top-level asm block becomes a module
 * asm block in the bitcode, which the final link assembles — the same
 * trick every runtime with hand-written assembly uses.
 *
 * The switch semantics (see runtime/rt_fiber.h and the plan in
 * design-docs/TASKING-FIBER-PLAN.md):
 *   void pli_fiber_switch(pli_fiber *from, pli_fiber *to);
 *   void pli_fiber_start(pli_fiber *f, void *stack_top, void *entry);
 *
 * The switch saves the callee-saved registers of `from` and jumps to `to`,
 * which resumes exactly where it left off. The saved set is the platform
 * ABI's callee-saved set, because a C function suspended inside the call
 * keeps its live values there:
 *   x86-64 SysV  — RBX RBP R12-R15
 *   x86-64 Win64 — RBX RBP RDI R12-R15 and XMM6-XMM15
 *   AArch64      — x19-x28, x29, x30 and the low 64 bits of v8-v15
 *
 * The saved stack pointer is the switch's ENTRY stack pointer, and the
 * word at that address holds the resume address: it is the return address
 * of the pli_fiber_switch call in the suspended code. The restore ends
 * with a RET, which pops that word and leaves the resumed code with
 * exactly the stack pointer it had at the call site. Saving the post-push
 * stack pointer instead would leave the resumed frame eight bytes off,
 * and jumping to the saved address instead of returning would leave the
 * return address on the stack. (AArch64 keeps the return address in x30
 * rather than on the stack; the equivalent contract is described in its
 * own block below.)
 *
 * A platform without a backend keeps the legacy OS-thread model, and the
 * switch reports it if it is reached.
 */

#if defined(__APPLE__)
#define RT_FIBER_SYM(name) "_" name
#else
#define RT_FIBER_SYM(name) name
#endif

#if defined(__x86_64__) && !defined(_WIN32)

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

#elif defined(__x86_64__) && defined(_WIN32)

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

#elif defined(__aarch64__)

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

#else

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

#endif
