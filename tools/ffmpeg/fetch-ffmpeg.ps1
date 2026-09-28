#requires -Version 5.1
<#
.SYNOPSIS
    Downloads a static Windows ffmpeg.exe build (with libx264 + SRT support) into
    ui/SrtEncoderApp/ffmpeg/ffmpeg.exe.

.DESCRIPTION
    ffmpeg runs as a subprocess (see native/CaptureCore/src/encode/FfmpegProcessController.cpp),
    not linked as a library — see docs/architecture.md for why. This fetches a prebuilt
    binary rather than requiring every developer to build ffmpeg from source.

    Source: BtbN's FFmpeg-Builds GitHub releases (GPL, includes libsrt).
    https://github.com/BtbN/FFmpeg-Builds

.PARAMETER Tag
    A specific BtbN release tag to pin to (see the releases page above), or "latest"
    (default) for their continuously-updated "latest" alias. Pin a specific tag for
    reproducible builds; "latest" is simplest for day-to-day dev.
#>
param(
    [string]$Tag = "latest"
)

$ErrorActionPreference = 'Stop'
# This script lives at tools/ffmpeg/ — up two levels, not one, to reach the repo root.
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$destDir = Join-Path $repoRoot "ui\SrtEncoderApp\ffmpeg"
$destExe = Join-Path $destDir "ffmpeg.exe"
$zipUrl = "https://github.com/BtbN/FFmpeg-Builds/releases/download/$Tag/ffmpeg-master-latest-win64-gpl.zip"

if (Test-Path $destExe) {
    Write-Host "ffmpeg.exe already present at $destExe - delete it first to re-fetch." -ForegroundColor Yellow
    exit 0
}

New-Item -ItemType Directory -Force -Path $destDir | Out-Null
$tempZip = Join-Path ([System.IO.Path]::GetTempPath()) "srtencoder-ffmpeg-download.zip"
$tempExtract = Join-Path ([System.IO.Path]::GetTempPath()) "srtencoder-ffmpeg-extract"

Write-Host "==> Downloading $zipUrl" -ForegroundColor Cyan
Invoke-WebRequest -Uri $zipUrl -OutFile $tempZip

Write-Host "==> Extracting" -ForegroundColor Cyan
if (Test-Path $tempExtract) { Remove-Item $tempExtract -Recurse -Force }
Expand-Archive -Path $tempZip -DestinationPath $tempExtract -Force

$exe = Get-ChildItem -Path $tempExtract -Recurse -Filter "ffmpeg.exe" | Select-Object -First 1
if (-not $exe) {
    throw "ffmpeg.exe not found in the downloaded archive - BtbN's archive layout may have changed."
}
Copy-Item $exe.FullName $destExe -Force

Remove-Item $tempZip -Force
Remove-Item $tempExtract -Recurse -Force

Write-Host "==> Done: $destExe" -ForegroundColor Green
