# Configure + build (+ optionally test) the engine from any PowerShell window.
# ASCII-only on purpose (Windows PowerShell 5.1 reads BOM-less files as ANSI).
#
#   .\scripts\build.ps1                       # Debug build
#   .\scripts\build.ps1 -Config Release -Test # Release build + unit tests + smoke test
#
# Why a script: Ninja + MSVC needs the "Developer PowerShell" environment
# (INCLUDE/LIB/PATH for cl.exe). This script enters it automatically via vswhere,
# so no manual setup and no global PATH changes are needed.

param(
    [ValidateSet('Debug', 'Release')] [string]$Config = 'Debug',
    [switch]$Test,
    [int]$SmokeFrames = 120
)
# 'Continue', not 'Stop': native tools (cmake, cl) print warnings to stderr, and PowerShell 5.1
# would turn those into terminating errors. Failures are detected via $LASTEXITCODE instead.
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio with the C++ workload was not found. Run scripts\check_env.ps1.' }
    Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}
if (-not $env:VULKAN_SDK) { $env:VULKAN_SDK = [Environment]::GetEnvironmentVariable('VULKAN_SDK', 'Machine') }

Push-Location $root
try {
    if (-not (Test-Path 'build\build.ninja')) {
        cmake --preset default
        if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
    }
    $preset = $Config.ToLower()
    cmake --build --preset $preset
    if ($LASTEXITCODE -ne 0) { throw "Build failed ($Config)" }

    if ($Test) {
        ctest --preset $preset
        if ($LASTEXITCODE -ne 0) { throw "Unit tests failed ($Config)" }
        # Smoke test: run N frames (with a window resize in the middle), validation forced on
        # even in Release. Exit code != 0 on any error log or validation error.
        & ".\build\bin\$Config\sandbox.exe" --frames $SmokeFrames --validation
        if ($LASTEXITCODE -ne 0) { throw "Smoke test failed ($Config), exit code $LASTEXITCODE" }
        Write-Host "All tests passed ($Config)." -ForegroundColor Green
    }
}
finally { Pop-Location }
