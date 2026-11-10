#!/usr/bin/env pwsh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# scripts/build.ps1 - one-command Windows console build (mirrors CI Windows steps).
#
# Generates a Ninja build and produces pli.exe. Run once:
#   .\scripts\build.ps1 [-Smoke] [-SkipZlibBuild]
#
# What it does (single command):
#   1. Runs vcvarsall.bat x64 to activate the VC dev shell (BuildTools first).
#   2. Adds mt.exe (Windows SDK) to PATH and pins CMAKE_MT for CMake.
#   3. Fetches the pinned static LLVM into $env:USERPROFILE\pli-deps (cached,
#      sha256-verified via scripts/fetch_llvm_static.ps1 + scripts/llvm_static.pins).
#   4. Builds zlib 1.3.1 static with the stash clang-cl under
#      $env:USERPROFILE\pli-tools\zlib (or $env:ZLIB_ROOT_DIR cache); skipped
#      with -SkipZlibBuild when the artifacts already exist.
#   5. Copies zlibstatic.lib to zlib-root/lib/{zlibstatic.lib,z.lib} and
#      $PREFIX\lib\zlibstatic.lib; copies zstd_static.lib -> zstd.lib (bare
#      names z/zstd for lld-link; identical to the CI "Build zlib" step).
#   6. cmake -G Ninja -B build/cmake ...           -> configures (the
#      mixed-CRT audit in CMakeLists.txt gate-keeps here)
#   7. cmake --build build/cmake -j                -> vs-free pli.exe
#   8. With -Smoke: python tests\run_tests.py smoke (expects 21/21 PASS).
#
# Result: build/cmake/pli.exe (self-contained /MT, no vcpkg or external DLLs).
#
# No Visual Studio .sln is generated: the VS generator + cl.exe path never
# compiles the Windows STL/compiler-rt stub branch (CMakeLists.txt gates it on
# CMAKE_CXX_COMPILER_ID MATCHES "Clang"), which leaves __std_* and __divti3
# unresolved. The Ninja + clang-cl path below is the one that links cleanly.
#
# CRT rule: everything linked from the static prefix must be MultiThreaded
# (/MT, LIBCMT/libcpmt). A single /MD archive inside the prefix makes the link
# fail with ~30 unresolved __std_* and __imp_* symbols; the configure-time
# mixed-CRT audit in CMakeLists.txt refuses that. zlibstatic.lib (shipped by
# CI with /DEFAULTLIB:msvcrt.lib) is fine - only LLVM/clang/lld archives carry
# the /MT contract.
#
# NOTE: keep this file pure ASCII (no em dashes, smart quotes, arrows).
# Windows PowerShell 5.1 reads BOM-less .ps1 files as ANSI, which corrupts
# multi-byte punctuation and can break string parsing.

param(
  [switch]$Smoke,                    # run tests\run_tests.py smoke after building
  [switch]$SkipZlibBuild             # reuse an existing zlib artifact tree
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$SELF_DIR = (Get-Item (Split-Path -Parent $MyInvocation.MyCommand.Path)).FullName
$REPO_ROOT = (Get-Item (Join-Path $SELF_DIR "..")).FullName
$PREFIX = Join-Path $env:USERPROFILE "pli-deps"
# Forward-slash form for cmake -D (avoids CMake mangling backslash escapes like \U).
$PREFIX_CMAKE = $PREFIX -replace '\\', '/'
$BUILD_DIR = Join-Path $REPO_ROOT "build/cmake"
$ZLIB_HOME = Join-Path $env:USERPROFILE "pli-tools\zlib"   # tarball, build tree, install tree

# 1. Activate VC dev shell (vcvarsall.bat x64). BuildTools first: Community's
#    toolset/vcxproj handling can shadow what the SDK/mt.exe pins expect.
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

# 2. mt.exe on PATH + explicit CMAKE_MT pin. Configure fails when CMake cannot
#    locate mt.exe (the VS generator inaccurately claims not to need it); the
#    SDK provides one mt.exe per bin root - pin the newest explicitly.
$sdkRoot = "C:\Program Files (x86)\Windows Kits\10\bin"
$mtExe = Get-ChildItem -Path $sdkRoot -Filter mt.exe -Recurse -ErrorAction SilentlyContinue |
  Where-Object { $_.Directory.Name -eq "x64" } |
  Sort-Object { [version]$_.Directory.Parent.Name } -Descending |
  Select-Object -First 1
if ($mtExe) {
  $env:PATH = "$($mtExe.Directory.FullName);$env:PATH"
  $CMAKE_MT = $mtExe.FullName -replace '\\', '/'
  Write-Host "build.ps1: mt.exe pinned -> $CMAKE_MT"
} else {
  Write-Warning "build.ps1: mt.exe not found under $sdkRoot - CMake must locate it (or configure fails)"
}

# 3. Fetch pinned static LLVM (once per machine; result is cached + hash-verified).
Write-Host "build.ps1: fetching pinned static LLVM into $PREFIX"
& "$SELF_DIR\fetch_llvm_static.ps1" -Prefix $PREFIX
$env:PATH = "$PREFIX\bin;$env:PATH"   # stash clang-cl + lld-link for host and zlib steps

# 4+5. zlib (the static LLVM libs reference adler32/compress2/...; zstd ships
#      inside the stash, zlib does not). Mirrors the CI "Build zlib" step.
$ZLIB_ROOT = Join-Path $ZLIB_HOME "zlib-root"
if ($env:ZLIB_ROOT_DIR) { $ZLIB_ROOT = $env:ZLIB_ROOT_DIR }   # CI-style override
if (-not $SkipZlibBuild) {
  New-Item -ItemType Directory -Force -Path $ZLIB_HOME | Out-Null
  Push-Location $ZLIB_HOME
  try {
    $ZLIB_VERSION = "1.3.1"
    $zlibSrc = Join-Path $ZLIB_HOME "zlib-$ZLIB_VERSION"
    if (-not (Test-Path $zlibSrc)) {
      Write-Host "build.ps1: fetching zlib $ZLIB_VERSION"
      $tarball = Join-Path $ZLIB_HOME "zlib-$ZLIB_VERSION.tar.xz"
      curl.exe -fL --retry 3 -o $tarball `
        "https://github.com/madler/zlib/releases/download/v$ZLIB_VERSION/zlib-$ZLIB_VERSION.tar.xz"
      tar -xf $tarball -C $ZLIB_HOME
    }
    Write-Host "build.ps1: configuring zlib (clang-cl, static, Release) ..."
    cmake -S $zlibSrc -B zlib-build -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_C_COMPILER=clang-cl -DZLIB_BUILD_SHARED=OFF -DZLIB_BUILD_STATIC=ON
    if ($LASTEXITCODE -ne 0) { Write-Error "build.ps1: zlib configure failed"; exit 1 }
    Write-Host "build.ps1: building zlib ..."
    cmake --build zlib-build -j $env:NUMBER_OF_PROCESSORS
    if ($LASTEXITCODE -ne 0) { Write-Error "build.ps1: zlib build failed"; exit 1 }
    # Same bare-name layout CI produces, plus zstd.lib for bare-name zstd.
    New-Item -ItemType Directory -Force -Path (Join-Path $ZLIB_ROOT "lib") | Out-Null
    $zlibstatic = Join-Path $ZLIB_HOME "zlib-build\zlibstatic.lib"
    Copy-Item $zlibstatic (Join-Path $ZLIB_ROOT "lib\zlibstatic.lib") -Force
    Copy-Item $zlibstatic (Join-Path $ZLIB_ROOT "lib\z.lib") -Force
    Copy-Item $zlibstatic (Join-Path $PREFIX "lib\zlibstatic.lib") -Force
    Copy-Item (Join-Path $PREFIX "lib\zstd_static.lib") (Join-Path $PREFIX "lib\zstd.lib") -Force
  } finally {
    Pop-Location
  }
}

# 6. Configure (Ninja + clang-cl; the mixed-CRT audit in CMakeLists.txt gate-keeps here).
Push-Location $REPO_ROOT
try {
  Write-Host "build.ps1: configuring (Ninja + clang-cl) ..."
  cmake -G Ninja -S . -B $BUILD_DIR `
    -DPLI_LLVM_STATIC_PREFIX="$PREFIX_CMAKE" `
    -DPLI_ZSTD_ROOT="$PREFIX_CMAKE" `
    -DPLI_ZLIB_ROOT="$ZLIB_ROOT" `
    -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
    -DCMAKE_BUILD_TYPE=Release
  if ($LASTEXITCODE -ne 0) {
    Write-Error "build.ps1: cmake configure failed (mixed-CRT audit FATALs name the offending lib)"
    exit 1
  }

  # 7. Build.
  Write-Host "build.ps1: building pli.exe ..."
  cmake --build $BUILD_DIR -j $env:NUMBER_OF_PROCESSORS
  if ($LASTEXITCODE -ne 0) {
    Write-Error "build.ps1: cmake build failed (exit $LASTEXITCODE)"
    exit 1
  }

  # 8. Smoke.
  if ($Smoke) {
    Write-Host "build.ps1: smoke test ..."
    $env:CLANG_RT_BUILTINS = Join-Path $PREFIX "lib\windows\clang_rt.builtins-x86_64.lib"
    python tests\run_tests.py smoke
    if ($LASTEXITCODE -ne 0) {
      Write-Error "build.ps1: smoke test failed"
      exit 1
    }
  }
} finally {
  Pop-Location
}

Write-Host "build.ps1: done -> $BUILD_DIR\pli.exe"
