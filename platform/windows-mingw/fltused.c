// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// platform/windows-mingw/fltused.c — source of the vendored fltused.o stub
// (ADR-227). clang targeting the MSVC ABI emits a `_fltused` reference in
// every object using floating point; the MSVC CRT normally provides it.
// The vendored mingw sysroot carries this one-word stub instead so the
// MinGW-driver link needs no MSVC library. Rebuild with:
//   x86_64-w64-mingw32-gcc -c fltused.c -o fltused.o
int _fltused = 0;
