#requires -Version 5.1
<#
.SYNOPSIS
    Publishes SimpleSrtEncoder and packages/uploads it as a Velopack release on GitHub.

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
    --azureTrustedSignFile). Default: azure-trusted-signing-metadata.json at the repo
    root, which reuses the same Azure Trusted Signing account + certificate profile as
    the Stadium Sound Electron app (see its electron-builder.yml's azureSignOptions) —
    one certificate profile can sign any number of different apps for the same
    publisher identity, so this isn't per-app config. Pass an empty string to produce
    an unsigned build instead (fine for local testing; unsigned output will trigger
    SmartScreen for end users).

    Either way, signing itself still needs AZURE_TENANT_ID/AZURE_CLIENT_ID/
    AZURE_CLIENT_SECRET in the environment (the same service-principal secrets
    Stadium Sound's release.yml passes to electron-builder) — vpk bundles Azure.Identity
    and picks them up automatically via DefaultAzureCredential; nothing to configure
    beyond having them set.

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

    # $null (not passed) picks the repo-root default below; pass '' explicitly to opt
    # out of signing instead ($PSScriptRoot isn't reliably populated yet this early, in
    # a parameter default expression, in Windows PowerShell 5.1 — resolved in the body).
    $AzureTrustedSignFile = $null,

    [string]$SignTemplate,

    [string]$RepoUrl,

    # $null (not passed) picks up $env:GITHUB_TOKEN below, AFTER .env has been loaded —
    # evaluating `$env:GITHUB_TOKEN` directly as the parameter default here would run at
    # parameter-binding time, before the .env-loading code further down ever executes,
    # so a token placed only in .env (not already in the process environment) would be
    # silently ignored.
    [string]$GithubToken = $null,

    [switch]$Publish,

    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$repoRoot = $PSScriptRoot
$appId = 'SimpleSrtEncoder'
$csproj = "$repoRoot/ui/SimpleSrtEncoder/SimpleSrtEncoder.csproj"
$publishDir = "$repoRoot/ui/SimpleSrtEncoder/bin/Release/net10.0-windows10.0.26100.0/win-x64/publish"
if ($null -eq $AzureTrustedSignFile) {
    $AzureTrustedSignFile = "$repoRoot/azure-trusted-signing-metadata.json"
}

# Load .env (gitignored — see .env.example) if present, without overwriting anything
# already set in the environment (so a real CI secret always wins over a stale .env).
$envFile = "$repoRoot/.env"
if (Test-Path $envFile) {
    Write-Host "==> Loading $envFile" -ForegroundColor Cyan
    foreach ($line in Get-Content $envFile) {
        if ($line -match '^\s*#' -or $line -notmatch '=') { continue }
        $key, $value = $line -split '=', 2
        $key = $key.Trim()
        if (-not (Get-Item "env:$key" -ErrorAction SilentlyContinue) -and $value) {
            [System.Environment]::SetEnvironmentVariable($key, $value.Trim())
        }
    }
}

if (-not $GithubToken) {
    $GithubToken = $env:GITHUB_TOKEN
}

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
dotnet publish $csproj -c Release -p:Platform=x64 -p:PublishProfile=win-x64 -p:Version=$Version -p:SelfContained=true -p:WindowsAppSDKSelfContained=true
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not (Test-Path "$publishDir/SimpleSrtEncoder.exe")) {
    throw "Publish output not found at $publishDir — check the PublishProfile/paths above still match SimpleSrtEncoder.csproj."
}

Write-Host "==> Packaging with vpk ($Version)" -ForegroundColor Cyan
$packArgs = @(
    'pack'
    '--packId', $appId
    '--packVersion', $Version
    '--packDir', $publishDir
    '--mainExe', 'SimpleSrtEncoder.exe'
    '--outputDir', $OutputDir
)
if ($AzureTrustedSignFile -and (Test-Path $AzureTrustedSignFile)) {
    foreach ($var in @('AZURE_TENANT_ID', 'AZURE_CLIENT_ID', 'AZURE_CLIENT_SECRET')) {
        if (-not (Get-Item "env:$var" -ErrorAction SilentlyContinue)) {
            throw "$AzureTrustedSignFile is set, but `$env:$var isn't — signing will fail with an Azure.Identity auth error otherwise. Set all three (AZURE_TENANT_ID/AZURE_CLIENT_ID/AZURE_CLIENT_SECRET), same as Stadium Sound's release.yml."
        }
    }
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
