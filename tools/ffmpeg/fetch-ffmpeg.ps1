#requires -Version 5.1
<#
.SYNOPSIS
    Downloads two static Windows ffmpeg builds into ui/SimpleSrtEncoder/ffmpeg/:
    ffmpeg.exe (primary) and ffmpeg-legacy-nvenc.exe (NVENC compatibility fallback).

.DESCRIPTION
    ffmpeg runs as a subprocess (see native/CaptureCore/src/encode/FfmpegProcessController.cpp),
    not linked as a library — see docs/architecture.md for why. This fetches prebuilt
    binaries rather than requiring every developer to build ffmpeg from source.

    Source: BtbN's FFmpeg-Builds GitHub releases (GPL, includes libsrt).
    https://github.com/BtbN/FFmpeg-Builds

    Two binaries, not one, because of a real compatibility conflict:
      - ffmpeg.exe tracks BtbN's master build (asset "ffmpeg-master-latest-...") — newest
        codecs/features/bugfixes, used by default for everything.
      - ffmpeg-legacy-nvenc.exe is pinned to BtbN's rolling 8.1.x branch build (asset
        "ffmpeg-n8.1-latest-...") and exists ONLY as an NVENC fallback. ffmpeg 9.0 bumped
        its bundled nv-codec-headers to require NVENC API 13.1 (NVIDIA driver >=610.00),
        which broke h264_nvenc on every Pascal-generation GPU (GTX 10-series, Quadro
        P-series, ...) — NVIDIA's R580 driver branch is the last one that supports Pascal
        at all, so no future driver update fixes this on that hardware. The 8.1.x branch
        still only needs NVENC API 13.0 (driver >=570), which those cards' actual drivers
        already satisfy.
    CaptureManager::ProbeEncoder (see MainViewModel.ResolveEncoder in the C# UI) tries
    h264_nvenc on the primary build first, then falls back to trying it on the legacy
    build, before giving up on hardware encoding and using libx264 (software, always on
    the primary build). Everything other than that narrow NVENC-on-old-hardware case
    always uses the primary build.

.PARAMETER Tag
    A specific BtbN release tag to pin to (see the releases page above), or "latest"
    (default) for their continuously-updated "latest" alias, which hosts both the master
    and n8.1 branch assets used here under the one tag.
#>
param(
    [string]$Tag = "latest"
)

$ErrorActionPreference = 'Stop'
# This script lives at tools/ffmpeg/ — up two levels, not one, to reach the repo root.
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$destDir = Join-Path $repoRoot "ui\SimpleSrtEncoder\ffmpeg"
New-Item -ItemType Directory -Force -Path $destDir | Out-Null

function Get-FfmpegBuild {
    param(
        [string]$AssetName,
        [string]$DestName,
        [string]$Description
    )

    $destExe = Join-Path $destDir $DestName
    if (Test-Path $destExe) {
        Write-Host "$DestName already present at $destExe - delete it first to re-fetch." -ForegroundColor Yellow
        return
    }

    $zipUrl = "https://github.com/BtbN/FFmpeg-Builds/releases/download/$Tag/$AssetName"
    $tempZip = Join-Path ([System.IO.Path]::GetTempPath()) "srtencoder-ffmpeg-download-$DestName.zip"
    $tempExtract = Join-Path ([System.IO.Path]::GetTempPath()) "srtencoder-ffmpeg-extract-$DestName"

    Write-Host "==> Downloading $Description`: $zipUrl" -ForegroundColor Cyan
    Invoke-WebRequest -Uri $zipUrl -OutFile $tempZip

    Write-Host "==> Extracting" -ForegroundColor Cyan
    if (Test-Path $tempExtract) { Remove-Item $tempExtract -Recurse -Force }
    Expand-Archive -Path $tempZip -DestinationPath $tempExtract -Force

    $exe = Get-ChildItem -Path $tempExtract -Recurse -Filter "ffmpeg.exe" | Select-Object -First 1
    if (-not $exe) {
        throw "ffmpeg.exe not found in $AssetName - BtbN's archive layout may have changed."
    }
    Copy-Item $exe.FullName $destExe -Force

    Remove-Item $tempZip -Force
    Remove-Item $tempExtract -Recurse -Force
    Write-Host "==> Done: $destExe" -ForegroundColor Green
}

Get-FfmpegBuild -AssetName "ffmpeg-master-latest-win64-gpl.zip" -DestName "ffmpeg.exe" -Description "primary (master)"
Get-FfmpegBuild -AssetName "ffmpeg-n8.1-latest-win64-gpl-8.1.zip" -DestName "ffmpeg-legacy-nvenc.exe" -Description "legacy NVENC fallback (8.1.x)"
