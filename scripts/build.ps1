#!/usr/bin/env pwsh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# scripts/build.ps1 — one-command console build.
#
# Generates a Ninja build and produces pli.exe. Run once:
#   .\scripts\build.ps1
#
# What it does (single command):
#   1. Runs vcvarsall.bat x64 to activate the VC dev shell.
#   2. Adds Ninja + Windows SDK bin (mt.exe) to PATH.
#   3. Fetches the pinned static LLVM into $env:USERPROFILE\pli-deps (cached).
#   4. cmake -G Ninja -B build/cmake ...           -> configures
#   5. cmake --build build/cmake -j                 -> build/vs-free pli.exe
#
# Result: build/cmake/pli.exe (self-contained, no vcpkg or external DLLs).
#
# No Visual Studio .sln is generated: the VS generator trips on MSVC STL 14.16
# (char8_t / std::max / PliI128 errors in the static LLVM headers). The Ninja +
# clang-cl path below is the one that compiles cleanly.

$ProgressPreference = "SilentlyContinue"

$SELF_DIR = (Get-Item (Split-Path -Parent $MyInvocation.MyCommand.Path)).FullName
$REPO_ROOT = (Get-Item (Join-Path $SELF_DIR "..")).FullName
$PREFIX = Join-Path $env:USERPROFILE "pli-deps"
# Forward-slash form for cmake -D (avoids CMake mangling backslash escapes like \U).
$PREFIX_CMAKE = $PREFIX -replace '\\', '/'
$BUILD_DIR = Join-Path $REPO_ROOT "build/cmake"

# 1. Activate VC dev shell (vcvarsall.bat x64).
$vcvarsCandidates = @(
  "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat",
  "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat",
  "C:\Program Files (x86)\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat",
  "C:\Program Files (x86)\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
)
$vcvars = $null
foreach ($c in $vcvarsCandidates) { if (Test-Path $c) { $vcvars = $c; break } }
if (-not $vcvars) {
  Write-Error "build.ps1: vcvarsall.bat not found; install Visual Studio 2022 Build Tools"
  exit 1
}

Write-Host "build.ps1: activating VC dev shell ($vcvars) ..."
$envVcvars = cmd /c "`"$vcvars`" x64 && set" 2>$null
foreach ($line in $envVcvars) {
  if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}
if (-not $env:VCToolsInstallDir) {
  Write-Error "build.ps1: vcvarsall.bat ran but VCToolsInstallDir is still unset"
  exit 1
}

# 2. Ninja + Windows SDK bin (mt.exe) on PATH.
$ninjaDir = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
$sdkBin   = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.22000.0\x64"
if (Test-Path $ninjaDir) { $env:PATH = "$ninjaDir;$env:PATH" }
if (Test-Path $sdkBin)   { $env:PATH = "$sdkBin;$env:PATH" }

# 3. Fetch pinned static LLVM (once per machine; result is cached).
Write-Host "build.ps1: fetching pinned static LLVM into $PREFIX"
& "$SELF_DIR\fetch_llvm_static.ps1" -Prefix $PREFIX
$env:PATH = "$PREFIX\bin;$env:PATH"   # clang-cl + lld-link

# 4. Configure (Ninja + clang-cl).
Push-Location $REPO_ROOT
try {
  Write-Host "build.ps1: configuring (Ninja + clang-cl) ..."
  cmake -G Ninja -S . -B $BUILD_DIR `
    -DPLI_LLVM_STATIC_PREFIX="$PREFIX_CMAKE" `
    -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
    -DCMAKE_BUILD_TYPE=Release
  if ($LASTEXITCODE -ne 0) {
    Write-Error "build.ps1: cmake configure failed (exit $LASTEXITCODE)"
    exit 1
  }

  # 5. Build.
  Write-Host "build.ps1: building pli.exe ..."
  cmake --build $BUILD_DIR -j
  if ($LASTEXITCODE -ne 0) {
    Write-Error "build.ps1: cmake build failed (exit $LASTEXITCODE)"
    exit 1
  }
} finally {
  Pop-Location
}

Write-Host "build.ps1: done -> $BUILD_DIR\pli.exe"
