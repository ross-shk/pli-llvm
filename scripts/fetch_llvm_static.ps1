#!/usr/bin/env pwsh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# scripts/fetch_llvm_static.ps1 — fetch + verify + extract the hermetic static
# LLVM stash (scripts/llvm_static.pins) into a prefix, without compiling.
#
# PowerShell port of scripts/fetch_llvm_static.sh. Same pins, same URLs, same
# sha256 trust anchor — single source of truth stays in llvm_static.pins.
#
# Usage: .\scripts\fetch_llvm_static.ps1 --prefix <dir>
# Env:   PLI_DEPS_TAG=<tag> override the release tag (testing/forks)
#        PLI_DEPS_URL=<base> override the release base URL

[CmdletBinding()]
param(
  [Parameter(Mandatory=$true)]
  [string]$Prefix
)

$ErrorActionPreference = "Stop"

# Resolve this script's dir regardless of cwd.
$SELF_DIR = (Get-Item (Split-Path -Parent $MyInvocation.MyCommand.Path)).FullName
$PINS = Join-Path $SELF_DIR "llvm_static.pins"
$PINS = (Resolve-Path $PINS).Path

# Read pins.
function Get-Pin([string]$Key) {
  $line = Get-Content $PINS | Where-Object { $_ -match "^$Key=" } | Select-Object -First 1
  if (-not $line) { return $null }
  return ($line -split "=", 2)[1]
}

$LLVM_VER = Get-Pin "LLVM_VER"
$DEPS_TAG = $env:PLI_DEPS_TAG
if (-not $DEPS_TAG) { $DEPS_TAG = Get-Pin "DEPS_TAG" }

# Platform name = release matrix key (must match release.yml + llvm-deps.yml).
$arch = $env:PROCESSOR_ARCHITECTURE
switch ($arch) {
  "AMD64" { $PLAT = "windows-x86_64" }
  "ARM64" { $PLAT = "windows-arm64" }
  default { throw "fetch_llvm_static.ps1: unsupported Windows arch ($arch)" }
}

$SHA = (Get-Content $PINS | Where-Object { $_ -match "^$PLAT\s" } | Select-Object -First 1) -split "\s+", 2 | Select-Object -Last 1
if (-not $LLVM_VER -or -not $DEPS_TAG) { throw "fetch_llvm_static.ps1: pins missing LLVM_VER/DEPS_TAG" }
if (-not $SHA) { throw "fetch_llvm_static.ps1: no stash pin for $PLAT (run llvm-deps promotion first)" }

# GitHub repo (from git remote) so forks/CI resolve the right release.
$REPO = $env:PLI_DEPS_REPO
if (-not $REPO) {
  $remote = git -C (Join-Path $SELF_DIR "..") config --get remote.origin.url 2>$null
  if ($remote -match '(.*github\.com[/:])([^/]+)/([^/.]+)(\.git)?$') {
    $REPO = "$($matches[2])/$($matches[3])"
  }
}
if (-not $REPO) { $REPO = "ross-shk/pli-llvm" }
$BASE = $env:PLI_DEPS_URL
if (-not $BASE) { $BASE = "https://github.com/$REPO/releases/download/$DEPS_TAG" }
$TARBALL = "llvm-static-$LLVM_VER-$PLAT.tar.zst"
$URL = "$BASE/$TARBALL"

# Local cache: one download shared across worktrees.
$cacheRoot = if ($env:XDG_CACHE_HOME) { $env:XDG_CACHE_HOME } else { Join-Path $env:HOME ".cache" }
$CACHE_DIR = Join-Path $cacheRoot "pli-deps"
$CACHE = Join-Path $CACHE_DIR $TARBALL
New-Item -ItemType Directory -Force -Path $CACHE_DIR | Out-Null
New-Item -ItemType Directory -Force -Path $Prefix | Out-Null

function Get-Sha256([string]$Path) {
  $hash = Get-FileHash -Path $Path -Algorithm SHA256
  return $hash.Hash.ToLower()
}

$needDownload = $true
if (Test-Path $CACHE) {
  $got = Get-Sha256 $CACHE
  if ($got -eq $SHA) {
    Write-Host "fetch_llvm_static.ps1: using cached $TARBALL"
    $needDownload = $false
  }
}

if ($needDownload) {
  Write-Host "fetch_llvm_static.ps1: downloading $URL"
  Invoke-WebRequest -Uri $URL -OutFile $CACHE -MaximumRetryCount 3
  $got = Get-Sha256 $CACHE
  if ($got -ne $SHA) {
    Write-Error "fetch_llvm_static.ps1: sha256 mismatch for $TARBALL (got $got, want $SHA)"
    Write-Error "stash changed on the server; remove the cached copy and retry, or bump pins"
    Remove-Item $CACHE -Force
    exit 1
  }
}

# Extract into the prefix (tarball contains the install-tree contents).
if (-not (Get-Command zstd -ErrorAction SilentlyContinue)) {
  throw "fetch_llvm_static.ps1: zstd needed to unpack (install zstd and add it to PATH)"
}
& zstd -dc $CACHE | tar -C $Prefix -xf -

Write-Host "fetch_llvm_static.ps1: extracted $LLVM_VER into $Prefix"
