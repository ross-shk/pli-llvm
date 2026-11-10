#!/usr/bin/env pwsh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# scripts/win_audit_prefix.ps1 - mixed-CRT audit of a static LLVM prefix.
#
# The configure-time guard in CMakeLists.txt FATALs when any LLVM/clang/lld
# archive inside PLI_LLVM_STATIC_PREFIX references only the dynamic CRT.
# This script is the standalone version of the same audit, for diagnosing a
# prefix (or validating it after fetch/build) without running CMake:
#
#   .\scripts\win_audit_prefix.ps1                       # audit $env:USERPROFILE\pli-deps
#   .\scripts\win_audit_prefix.ps1 -Prefix C:\my\deps    # audit a custom prefix
#   .\scripts\win_audit_prefix.ps1 -Lib C:\path\to\Foo.lib   # audit one archive
#
# Theory of operation: the COFF directive block (.drectve) of each member
# carries /DEFAULTLIB: tokens; cl emits the quoted mixed-case form
# (/DEFAULTLIB:"LIBCMT", /DEFAULTLIB:"libcpmt") while clang-cl/lld archives
# carry the .lib-spelled form (/DEFAULTLIB:msvcrt.lib). Decode the archive as
# Latin-1 (byte<->char 1:1 - no NUL truncation, unlike CMake file(READ)) and
# regex the directives, then classify: LIBCMT/LIBCMTD/LIBCPMT[/D] is static,
# MSVCRT/MSVCRTD/MSVCPRT[/D] is dynamic. A library that is dynamic-only does
# not satisfy the /MT contract and shows up in the offenders list (exit 1).
# zlibstatic.lib / z.lib (shipped by CI) and zstd archives are out of scope:
# only LLVM*/lld*/clang* archives inside the prefix carry the contract.

param(
  # Prefix whose lib\ directory is scanned (LLVM*/lld*/clang* archives).
  [string]$Prefix = (Join-Path $env:USERPROFILE "pli-deps"),
  # Single archive to audit; wins over -Prefix.
  [string]$Lib
)

$LF = "-" * 72
function Get-DefaultLibs {
  param([string]$Path)
  # Latin-1 decode keeps every byte; .NET strings survive NULs (unlike CMake
  # file(READ), which truncates archives at the first NUL - the trap that
  # originally hid these directives).
  $bytes = [IO.File]::ReadAllBytes($Path)
  $text = [Text.Encoding]::GetEncoding(28591).GetString($bytes)
  # /DEFAULTLIB:"FOO" or /DEFAULTLIB:FOO.lib - token spans to the next space.
  $out = @()
  foreach ($m in [regex]::Matches($text, '/DEFAULTLIB\s*:\s*("[^"]*"|\S+)')) {
    $tok = $m.Groups[1].Value.Trim(' " ')
    $tok = $tok -replace '\.LIB$', ''
    if ($tok) { $out += $tok.ToUpperInvariant() }
  }
  $out | Select-Object -Unique
}

function Test-Crt {
  param([string[]]$Directives)
  $static = @($Directives | Where-Object { $_ -match '^(LIBCMT|LIBCPMT)D?$' })
  $dyn    = @($Directives | Where-Object { $_ -match '^MSVC(RT|PRT)D?$' })
  @{ static = $static; dynamic = $dyn }
}

if ($Lib) {
  if (-not (Test-Path $Lib)) { Write-Error "win_audit_prefix.ps1: no such file: $Lib"; exit 2 }
  $targets = @((Get-Item $Lib).FullName)
  $contract = $false
} else {
  $libDir = Join-Path $Prefix "lib"
  $contract = $true
  $targets = @(Get-ChildItem -Path $libDir -Filter *.lib -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^(LLVM|lld|clang)[A-Za-z0-9]+\.lib$' } |
    Sort-Object FullName | ForEach-Object FullName)
  if (-not $targets) {
    Write-Error "win_audit_prefix.ps1: no LLVM*/lld*/clang* archives under $libDir (wrong prefix?)"
    exit 2
  }
}

$offenders = @()
foreach ($t in $targets) {
  $dirs = @(Get-DefaultLibs $t)
  $crt = Test-Crt $dirs
  $tag = if ($crt.dynamic.Count) { "DYNAMIC" } elseif ($crt.static.Count) { "static" } else { "none" }
  $dynNote = if ($crt.dynamic.Count) { " (msvc: $($crt.dynamic -join ','))" } else { "" }
  Write-Host ("  {0,-12} {1}{2}" -f $tag, (Split-Path $t -Leaf), $dynNote)
  if ($contract -and $crt.dynamic.Count -and -not $crt.static.Count) {
    $offenders += $t
  }
}

if ($contract -and $offenders) {
  Write-Host ""
  Write-Host "FATAL: dynamic-CRT-only archives inside $Prefix\lib :"
  foreach ($o in $offenders) { Write-Host "    $o" }
  Write-Host "Remedy: re-extract the pinned toolchain (scripts/fetch_llvm_static.ps1 -Prefix <prefix>);"
  Write-Host "if you rebuild LLVM by hand pass -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded."
  exit 1
}
Write-Host ""
if ($contract) {
  Write-Host "OK: $($targets.Count) archives audited, all MultiThreaded (or CRT-free)."
} else {
  Write-Host "Done: 1 archive audited (informational, single-lib mode)."
}
