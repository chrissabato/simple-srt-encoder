#requires -Version 5.1
<#
.SYNOPSIS
    Publishes SrtEncoderApp and packages/uploads it as a Velopack release on GitHub.

.DESCRIPTION
    1. Runs build.ps1 (native + UI), unless -SkipBuild.
    2. `dotnet publish`es the win-x64 self-contained profile.
    3. Runs `vpk pack` over that publish output, producing a Setup.exe and an updated
       releases.win.json/.nupkg feed under -OutputDir.
    4. If -RepoUrl is given, runs `vpk upload github` to push that release's assets to a
       GitHub Release (matching UpdateService.cs's GithubSource) — otherwise the packaged
       release is just left on disk under -OutputDir for you to upload yourself.

    vpk (the Velopack CLI) must already be installed: `dotnet tool install -g vpk`.

.PARAMETER Version
    The release version (semver, e.g. "1.0.0"). Required.

.PARAMETER OutputDir
    Where vpk writes the installer + update feed. Default: releases/.

.PARAMETER AzureTrustedSignFile
    Path to the Azure Trusted Signing metadata.json (vpk's native
    --azureTrustedSignFile), matching the Azure signing pipeline already used for the
    Electron app. Omit to produce an unsigned build — fine for local testing, but
    unsigned output will trigger SmartScreen for end users.

.PARAMETER SignTemplate
    Alternative to -AzureTrustedSignFile: an arbitrary signing command passed to vpk's
    --signTemplate, with {{file}} substituted for each file to sign.

.PARAMETER RepoUrl
    The GitHub repo to upload this release to (e.g. https://github.com/owner/repo). Must
    match UpdateService.cs's GithubRepoUrl. Omit to skip uploading — vpk pack still runs,
    the release just stays local under -OutputDir.

.PARAMETER GithubToken
    OAuth token for the upload (needs write access to the repo's Releases, even though
    the repo and its releases can be public for anyone to read). Defaults to
    $env:GITHUB_TOKEN. Required when -RepoUrl is given.

.PARAMETER Publish
    Publish the GitHub release immediately instead of leaving it as a draft (vpk upload
    github's own --publish). Default: leaves it as a draft so you can review before
    end users' apps pick it up.

.PARAMETER SkipBuild
    Skip the build.ps1 step (e.g. if you already built Release just now).
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$Version,

    [string]$OutputDir = 'releases',

    [string]$AzureTrustedSignFile,

    [string]$SignTemplate,

    [string]$RepoUrl,

    [string]$GithubToken = $env:GITHUB_TOKEN,

    [switch]$Publish,

    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$repoRoot = $PSScriptRoot
$appId = 'SrtEncoderApp'
$csproj = "$repoRoot/ui/SrtEncoderApp/SrtEncoderApp.csproj"
$publishDir = "$repoRoot/ui/SrtEncoderApp/bin/x64/Release/net10.0-windows10.0.26100.0/win-x64/publish"

if (-not (Get-Command vpk -ErrorAction SilentlyContinue)) {
    Write-Warning "vpk (the Velopack CLI) isn't on PATH. Install it with: dotnet tool install -g vpk"
    exit 1
}

if ($RepoUrl -and -not $GithubToken) {
    throw "-RepoUrl was given but no -GithubToken (or `$env:GITHUB_TOKEN) is set."
}

if (-not $SkipBuild) {
    Write-Host "==> Building native/ + ui/ (Release)" -ForegroundColor Cyan
    & "$repoRoot/build.ps1" -Configuration Release
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
else {
    Write-Host "==> Skipping build.ps1 (-SkipBuild)" -ForegroundColor Yellow
}

Write-Host "==> Publishing win-x64 (self-contained)" -ForegroundColor Cyan
dotnet publish $csproj -c Release -p:Platform=x64 -p:PublishProfile=win-x64 -p:Version=$Version
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not (Test-Path "$publishDir/SrtEncoderApp.exe")) {
    throw "Publish output not found at $publishDir — check the PublishProfile/paths above still match SrtEncoderApp.csproj."
}

Write-Host "==> Packaging with vpk ($Version)" -ForegroundColor Cyan
$packArgs = @(
    'pack'
    '--packId', $appId
    '--packVersion', $Version
    '--packDir', $publishDir
    '--mainExe', 'SrtEncoderApp.exe'
    '--outputDir', $OutputDir
)
if ($AzureTrustedSignFile) {
    $packArgs += @('--azureTrustedSignFile', $AzureTrustedSignFile)
}
elseif ($SignTemplate) {
    $packArgs += @('--signTemplate', $SignTemplate)
}
else {
    Write-Warning "No -AzureTrustedSignFile/-SignTemplate given — this release will be unsigned (SmartScreen will flag it for end users)."
}
vpk @packArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($RepoUrl) {
    Write-Host "==> Uploading to $RepoUrl" -ForegroundColor Cyan
    $uploadArgs = @(
        'upload', 'github'
        '--outputDir', $OutputDir
        '--repoUrl', $RepoUrl
        '--token', $GithubToken
        '--tag', "v$Version"
    )
    if ($Publish) {
        $uploadArgs += '--publish'
    }
    vpk @uploadArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Write-Host "==> Done. Release $(if (-not $Publish) { '(draft) ' })uploaded to $RepoUrl." -ForegroundColor Green
}
else {
    Write-Host "==> Done. Packaged under '$OutputDir' — re-run with -RepoUrl to upload to GitHub." -ForegroundColor Green
}
