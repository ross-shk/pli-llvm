# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# platform/windows-mingw/chkstk.s -- source of the vendored chkstk.o thunk
# (ADR-227). Dynamic AUTOMATIC arrays lower to a runtime-sized alloca whose
# LLVM prologue calls the GNU-named stack probe `__chkstk`, while the
# vendored libgcc.a only provides the MSVC-named `___chkstk_ms`. Both probes
# share the contract (size in %rax, touch every page down to %rsp-%rax,
# preserve registers), so the thunk is an exact tail-call. Rebuild with:
#   x86_64-w64-mingw32-gcc -c chkstk.s -o chkstk.o
    .globl __chkstk
    .text
__chkstk:
    jmp ___chkstk_ms
