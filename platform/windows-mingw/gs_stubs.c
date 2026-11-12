/*
 * GS security cookie stubs for Windows user-program links.
 *
 * compiler-rt builtins (pulled in for __udivti3 on i128 arithmetic) are
 * compiled with /GS and reference __security_cookie / __security_check_cookie
 * from MSVC's CRT. These stubs satisfy the link without pulling in libcmt.lib,
 * which would duplicate __guard_dispatch_icall_fptr from libmingwex.a.
 * Embedded as gs_stubs.o into the Windows link sysroot (ADR-227), so every
 * user program linked by pli.exe gets them.
 */
#include <stdint.h>

uintptr_t __security_cookie = 0x2B992DDFA23249D6ULL;

void __security_check_cookie(uintptr_t cookie) {
    (void)cookie;
}
