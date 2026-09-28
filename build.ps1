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

function Find-VsDevCmd {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        return $null
    }
    $vsInstallPath = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsInstallPath) {
        return $null
    }
    $vsDevCmd = Join-Path $vsInstallPath "Common7\Tools\VsDevCmd.bat"
    if (Test-Path $vsDevCmd) { return $vsDevCmd }
    return $null
}

if (-not $SkipNative) {
    # `cmake --build --preset` (unlike configure) has no -S flag — it looks for
    # CMakePresets.json relative to the current directory, so both calls need to run
    # from native/, not the repo root.
    $hasCmakeOnPath = [bool](Get-Command cmake.exe -ErrorAction SilentlyContinue)

    if ($hasCmakeOnPath) {
        Write-Host "==> Configuring native/ ($preset)" -ForegroundColor Cyan
        Push-Location "$repoRoot/native"
        try {
            cmake --preset $preset
            if ($LASTEXITCODE -ne 0) { throw "cmake configure failed (exit $LASTEXITCODE)" }
            Write-Host "==> Building native/ ($preset)" -ForegroundColor Cyan
            cmake --build --preset $preset
            if ($LASTEXITCODE -ne 0) { throw "cmake build failed (exit $LASTEXITCODE)" }
        }
        finally {
            Pop-Location
        }
    }
    else {
        # cmake/ninja/cl.exe live under the VS install tree, not the global PATH,
        # unless this script is already running inside a "Developer PowerShell for VS"
        # session. Run the whole native build inside one cmd.exe subprocess that
        # sources VsDevCmd.bat first — deliberately NOT importing those environment
        # variables into *this* PowerShell process, since VsDevCmd.bat sets things
        # (e.g. a bare `Platform` variable) that break the `dotnet build` step below
        # if they leak into it.
        $vsDevCmd = Find-VsDevCmd
        if (-not $vsDevCmd) {
            Write-Warning "cmake.exe not found, and no Visual Studio install with the C++ workload was found either. Install the 'Desktop development with C++' workload in Visual Studio (brings CMake + Ninja), then re-run, or pass -SkipNative to build the UI only."
            exit 1
        }

        Write-Host "==> Configuring + building native/ ($preset) via $vsDevCmd" -ForegroundColor Cyan
        $nativeDir = (Resolve-Path "$repoRoot/native").Path
        # VsDevCmd.bat internally shells out to a bare "vswhere.exe"; add its directory
        # to PATH for this subprocess only (scoped via `set`, not $env:PATH, so it
        # doesn't leak into the rest of this script).
        $vswhereDir = Split-Path "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -Parent
        $cmd = "set `"PATH=$vswhereDir;%PATH%`" && call `"$vsDevCmd`" -arch=x64 -no_logo && cd /d `"$nativeDir`" && cmake --preset $preset && cmake --build --preset $preset"
        cmd /c $cmd
        if ($LASTEXITCODE -ne 0) { throw "native build failed (exit $LASTEXITCODE)" }
    }
}
else {
    Write-Host "==> Skipping native build (-SkipNative)" -ForegroundColor Yellow
}

Write-Host "==> Building ui/SrtEncoderApp ($Configuration)" -ForegroundColor Cyan
dotnet build "$repoRoot/ui/SrtEncoderApp/SrtEncoderApp.csproj" -c $Configuration -p:Platform=x64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "==> Done." -ForegroundColor Green
