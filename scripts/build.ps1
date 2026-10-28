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

# Header dependency tracking (Ninja + MSVC) parses the "/showIncludes" lines that cl.exe prints. A localized
# cl.exe prints that prefix in the console code page, and CMake records the prefix once, at first configure.
# Building later in a console with another code page (e.g. a UTF-8 terminal) does not fail: Ninja silently
# records NO header dependencies, so editing a header no longer rebuilds the .cpp files that include it
# (stale objects with mismatched struct layouts -> memory corruption at run time; this happened once).
# So pin the console to the system OEM code page for the whole configure + build, and restore it afterwards.
$oemCodePage = [Globalization.CultureInfo]::CurrentCulture.TextInfo.OEMCodePage
$savedCodePage = [Console]::OutputEncoding.CodePage
$savedEncoding = [Console]::OutputEncoding
& chcp.com $oemCodePage | Out-Null
try { [Console]::OutputEncoding = [Text.Encoding]::GetEncoding($oemCodePage) } catch { }

Push-Location $root
try {
    & (Join-Path $PSScriptRoot 'fix_shader_encoding.ps1')
    if (-not (Test-Path 'build\build.ninja')) {
        cmake --preset default
        if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
    }
    $preset = $Config.ToLower()
    cmake --build --preset $preset
    if ($LASTEXITCODE -ne 0) { throw "Build failed ($Config)" }

    # Sanity check for the problem described above: sandbox's main.cpp includes hundreds of headers.
    if (Get-Command ninja.exe -ErrorAction SilentlyContinue) {
        $deps = & ninja.exe -C build -f "build-$Config.ninja" -t deps "sandbox/CMakeFiles/sandbox.dir/$Config/main.cpp.obj" 2>$null |
            Select-Object -First 1
        if ($deps -match '#deps 0,') {
            throw ("Ninja recorded no header dependencies (cl /showIncludes prefix does not match the console code page). " +
                   "Incremental builds would miss header changes. Reconfigure from a fresh build directory.")
        }
    }

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
finally {
    Pop-Location
    & chcp.com $savedCodePage | Out-Null
    try { [Console]::OutputEncoding = $savedEncoding } catch { }
}
