# Environment check for the engine (M0).
# Kept ASCII-only on purpose: Windows PowerShell 5.1 reads BOM-less files as ANSI,
# so non-ASCII text in scripts can break parsing on Chinese-locale systems.
# Usage: .\scripts\check_env.ps1      (exit code 0 = everything required is present)

$ErrorActionPreference = 'Continue'
$missing = 0

function Report($name, $ok, $detail, [switch]$Optional) {
    if ($ok) { Write-Host ("[ OK ] {0,-22} {1}" -f $name, $detail) -ForegroundColor Green }
    elseif ($Optional) { Write-Host ("[WARN] {0,-22} {1}" -f $name, $detail) -ForegroundColor Yellow }
    else { Write-Host ("[MISS] {0,-22} {1}" -f $name, $detail) -ForegroundColor Red; $script:missing++ }
}

# Git
$git = Get-Command git -ErrorAction SilentlyContinue
Report 'Git' ($null -ne $git) $(if ($git) { (git --version) } else { 'install from https://git-scm.com' })

# Visual Studio (any edition with the C++ workload). vswhere ships with the VS installer.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = $null
if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
Report 'Visual Studio + MSVC' ([bool]$vs) $(if ($vs) { $vs } else { 'install VS 2022+ with "Desktop development with C++"' })

# CMake + Ninja: prefer the copies bundled with VS so nothing else is needed on PATH.
if ($vs) {
    $cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    $ninja = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
    $cmakeVer = if (Test-Path $cmake) { (& $cmake --version | Select-Object -First 1) } else { '' }
    Report 'CMake (>= 3.25)' (Test-Path $cmake) $cmakeVer
    Report 'Ninja' (Test-Path $ninja) $ninja
}

# Vulkan SDK: needed for validation layers and dxc.
$sdk = $env:VULKAN_SDK
if (-not $sdk) { $sdk = [Environment]::GetEnvironmentVariable('VULKAN_SDK', 'Machine') }
Report 'Vulkan SDK' ([bool]$sdk -and (Test-Path "$sdk")) $(if ($sdk) { $sdk } else { 'install from https://vulkan.lunarg.com/sdk/home' })
if ($sdk) {
    Report 'dxc (HLSL -> SPIR-V)' (Test-Path "$sdk\Bin\dxc.exe") "$sdk\Bin\dxc.exe"
    Report 'Validation layer' (Test-Path "$sdk\Bin\VkLayer_khronos_validation.json") 'VK_LAYER_KHRONOS_validation'
}

# Vulkan runtime (installed by the GPU driver)
$vk = 'C:\Windows\System32\vulkan-1.dll'
Report 'Vulkan runtime' (Test-Path $vk) $(if (Test-Path $vk) { 'vulkan-1.dll ' + (Get-Item $vk).VersionInfo.ProductVersion } else { 'update GPU driver' })

# RenderDoc is optional: the engine just skips in-app capture when it is absent.
$rd = 'C:\Program Files\RenderDoc\renderdoc.dll'
Report 'RenderDoc (optional)' (Test-Path $rd) $(if (Test-Path $rd) { $rd } else { 'https://renderdoc.org (capture hotkey disabled without it)' }) -Optional

# GPU list: laptops have two GPUs, the engine must pick the discrete one.
Get-CimInstance Win32_VideoController | ForEach-Object { Write-Host ("       GPU: {0}  (driver {1})" -f $_.Name, $_.DriverVersion) }

if ($missing -gt 0) { Write-Host "$missing required item(s) missing." -ForegroundColor Red; exit 1 }
Write-Host 'Environment OK.' -ForegroundColor Green
exit 0
