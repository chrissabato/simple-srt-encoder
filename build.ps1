#requires -Version 5.1
<#
.SYNOPSIS
    Builds the native CaptureCore engine and the WinUI 3 app.

.DESCRIPTION
    Two independent build systems (CMake for native/, MSBuild/dotnet for ui/) are tied
    together here rather than merged into one solution, since VS's native-CMake and
    MSBuild C# project integrations don't compose cleanly.

    1. Configures + builds native/CaptureCore via CMake presets.
    2. Builds ui/SrtEncoderApp via `dotnet build`, which copies the resulting
       CaptureCore.dll into the app's output directory (see SrtEncoderApp.csproj's
       CopyNativeCore target).

.PARAMETER Configuration
    Debug or Release. Default: Debug.

.PARAMETER SkipNative
    Skip the CMake configure/build step (e.g. if the C++ toolchain isn't installed yet;
    the C# app still builds and runs, showing a fallback message in place of the native
    version string).
#>
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [switch]$SkipNative
)

$ErrorActionPreference = 'Stop'
$repoRoot = $PSScriptRoot
$preset = if ($Configuration -eq 'Debug') { 'x64-debug' } else { 'x64-release' }

if (-not $SkipNative) {
    $cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if (-not $cmake) {
        Write-Warning "cmake.exe not found on PATH. Install the 'Desktop development with C++' workload in Visual Studio (brings CMake + Ninja), then re-run, or pass -SkipNative to build the UI only."
        exit 1
    }

    Write-Host "==> Configuring native/ ($preset)" -ForegroundColor Cyan
    cmake --preset $preset -S "$repoRoot/native"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Host "==> Building native/ ($preset)" -ForegroundColor Cyan
    cmake --build --preset $preset
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
else {
    Write-Host "==> Skipping native build (-SkipNative)" -ForegroundColor Yellow
}

Write-Host "==> Building ui/SrtEncoderApp ($Configuration)" -ForegroundColor Cyan
dotnet build "$repoRoot/ui/SrtEncoderApp/SrtEncoderApp.csproj" -c $Configuration -p:Platform=x64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "==> Done." -ForegroundColor Green
