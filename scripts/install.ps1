#!/usr/bin/env pwsh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# scripts/install.ps1 - one-line Windows install for the pli compiler.
#
# Usual usage (fetched from the latest release and executed):
#   irm https://github.com/ross-shk/pli-llvm/releases/latest/download/install.ps1 | iex
#
# Or directly:
#   pwsh -ExecutionPolicy Bypass -File scripts/install.ps1 [-Version <tag>]
#
# What it does:
#   1. Resolves the latest release tag (or uses -Version) and builds the
#      pli-windows-x86_64.zip + .sha256 download URLs.
#   2. Downloads the zip to a temp dir and verifies its sha256 unless
#      -SkipVerify (verification requires the per-tag .sha256 asset shipped
#      by the release workflow; if it is missing the install fails fast and
#      points at -SkipVerify).
#   3. Extracts pli.exe (it lives inside a windows-x86_64/ subfolder in the
#      zip) and copies it to $DestDir\pli.exe.
#   4. Appends $DestDir to the USER-scope PATH only if it is not already
#      present (idempotent, no duplicates). Use -NoPath to skip the PATH step.
#
# Params:
#   -Version      exact release tag, e.g. v0.7.0 (default: latest).
#   -DestDir      install directory (default: $env:USERPROFILE\pli).
#   -Clean        remove an existing pli.exe before installing.
#   -SkipVerify   do not check the zip sha256.
#   -NoPath       do not touch the user PATH.
#
# NOTE: keep this file pure ASCII (no em dashes, smart quotes, arrows).
# Windows PowerShell 5.1 reads BOM-less .ps1 files as ANSI, which corrupts
# multi-byte punctuation and can break string parsing.

param(
  [string]$Version,
  [string]$DestDir,
  [switch]$Clean,
  [switch]$SkipVerify,
  [switch]$NoPath
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$REPO = "ross-shk/pli-llvm"
$ZIP_NAME = "pli-windows-x86_64.zip"
$BASE_URL = "https://github.com/$REPO/releases"

if (-not $DestDir) { $DestDir = Join-Path $env:USERPROFILE "pli" }

Write-Host "pli installer: installing to $DestDir"

# ---------------------------------------------------------------------------
# 1. Resolve the tag and the download URLs.
# ---------------------------------------------------------------------------
if ($Version) {
  $tag = $Version
} else {
  Write-Host "pli installer: resolving latest release tag ..."
  try {
    $latest = Invoke-RestMethod -Uri "https://api.github.com/repos/$REPO/releases/latest" `
      -Headers @{ "User-Agent" = "pli-installer" }
    $tag = $latest.tag_name
  } catch {
    Write-Error "pli installer: failed to resolve latest release: $($_.Exception.Message)"
    exit 1
  }
}
Write-Host "pli installer: release tag = $tag"

$zipUrl = "$BASE_URL/download/$tag/$ZIP_NAME"
$shaUrl = "$BASE_URL/download/$tag/$ZIP_NAME.sha256"

# ---------------------------------------------------------------------------
# 2. Download to a temp dir (with the tag in the name so parallel installs do
#    not collide), then verify.
# ---------------------------------------------------------------------------
$tmpDir = Join-Path $env:TEMP "pli-install-$tag"
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null
$zipPath = Join-Path $tmpDir $ZIP_NAME

Write-Host "pli installer: downloading $zipUrl"
try {
  Invoke-WebRequest -Uri $zipUrl -OutFile $zipPath
} catch {
  Write-Error "pli installer: download failed: $($_.Exception.Message)"
  exit 1
}

if (-not $SkipVerify) {
  Write-Host "pli installer: verifying sha256 ..."
  $shaPath = Join-Path $tmpDir "$ZIP_NAME.sha256"
  try {
    Invoke-WebRequest -Uri $shaUrl -OutFile $shaPath
  } catch {
    Write-Error "pli installer: could not fetch checksum ($shaUrl). Re-run with -SkipVerify to bypass integrity checking."
    exit 1
  }
  $expected = (Get-Content $shaPath -Raw).Trim() -split '\s+' | Select-Object -First 1
  $actual = (Get-FileHash -Path $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($expected.ToLowerInvariant() -ne $actual) {
    Write-Error "pli installer: sha256 mismatch. Expected $expected, got $actual. The download is corrupt or tampered with."
    exit 1
  }
  Write-Host "pli installer: sha256 OK"
}

# ---------------------------------------------------------------------------
# 3. Extract pli.exe. It lives in a windows-x86_64/ subfolder in the zip, so
#    search for it rather than assuming a top-level path.
# ---------------------------------------------------------------------------
$extractDir = Join-Path $tmpDir "extract"
if (Test-Path $extractDir) { Remove-Item $extractDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $extractDir | Out-Null
try {
  Expand-Archive -Path $zipPath -DestinationPath $extractDir -Force
} catch {
  Write-Error "pli installer: extraction failed: $($_.Exception.Message)"
  exit 1
}

$found = Get-ChildItem -Path $extractDir -Recurse -Filter "pli.exe" -File |
  Select-Object -First 1
if (-not $found) {
  Write-Error "pli installer: pli.exe not found inside the archive"
  exit 1
}

New-Item -ItemType Directory -Force -Path $DestDir | Out-Null
$dst = Join-Path $DestDir "pli.exe"
if ($Clean -and (Test-Path $dst)) {
  Remove-Item $dst -Force
  Write-Host "pli installer: removed existing $dst"
}
Copy-Item $found.FullName $dst -Force
Write-Host "pli installer: installed $dst"

# ---------------------------------------------------------------------------
# 4. User-scope PATH, idempotent (no duplicates).
# ---------------------------------------------------------------------------
if (-not $NoPath) {
  $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
  $entries = @($userPath -split ';' | Where-Object { $_ -ne "" })
  $already = $entries | Where-Object { $_.TrimEnd('\') -ieq $DestDir.TrimEnd('\') }
  if ($already) {
    Write-Host "pli installer: $DestDir already on PATH"
  } else {
    $newPath = if ($userPath) { "$userPath;$DestDir" } else { $DestDir }
    [Environment]::SetEnvironmentVariable("Path", $newPath, "User")
    Write-Host "pli installer: added $DestDir to user PATH"
  }
} else {
  Write-Host "pli installer: skipped PATH changes (-NoPath)"
}

# ---------------------------------------------------------------------------
# 5. Confirm with --version.
# ---------------------------------------------------------------------------
try {
  $ver = & $dst --version
  if ($LASTEXITCODE -ne 0) {
    Write-Error "pli installer: installed binary failed its --version check (exit $LASTEXITCODE)"
    exit 1
  }
  foreach ($line in $ver) { Write-Host "  $line" }
} catch {
  Write-Error "pli installer: could not run $dst --version: $($_.Exception.Message)"
  exit 1
}

Write-Host ""
Write-Host "pli installed to: $dst"
Write-Host "Open a NEW shell so $DestDir is on PATH, then run: pli --help"
exit 0
