@echo off
REM SPDX-License-Identifier: Apache-2.0
REM Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
REM Windows wrapper for the PL/I benchmark harness (cf. run_benchmarks.sh).
REM Optional: set LLVM_BIN to a directory holding clang.exe (or put clang.exe
REM on PATH). PLI and CC are honoured when already set; the runner otherwise
REM finds the newest build\pli.exe or build\cmake\pli.exe.
setlocal
set "DIR=%~dp0"
if defined LLVM_BIN set "PATH=%LLVM_BIN%;%PATH%"
if not defined CC set "CC=clang.exe"
REM Force UTF-8: the harness prints check marks the ANSI code page cannot encode.
set "PYTHONIOENCODING=utf-8"
set "PYTHONUTF8=1"
python "%DIR%run_benchmarks.py" %*
endlocal
