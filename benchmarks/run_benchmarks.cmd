@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
set "LLVM_BIN=C:\Users\Nick\Development\clang+llvm-23.1.3-x86_64-pc-windows-msvc\bin"
set "PATH=%LLVM_BIN%;%PATH%"
set "CC=clang.exe"
set "PYTHONIOENCODING=utf-8"
set "PYTHONUTF8=1"
python "C:\Users\Nick\Development\pli-llvm\benchmarks\run_benchmarks.py" %*