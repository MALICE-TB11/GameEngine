# Ensure every shader source (.hlsl/.hlsli) is saved as UTF-8 *with BOM*.
#
# Why: shaders are compiled with -Zi -Qembed_debug so RenderDoc can step through the HLSL
# source. On Chinese-locale Windows, dxc fails with "error code 0x80070459" when it embeds a
# BOM-less UTF-8 file containing non-ASCII text (e.g. Chinese comments). With a BOM it works.
# build.ps1 runs this automatically; .editorconfig asks editors to keep the BOM.

$root = Split-Path -Parent $PSScriptRoot
$utf8Bom = New-Object System.Text.UTF8Encoding $true
$fixed = 0
Get-ChildItem -Path (Join-Path $root 'shaders') -Recurse -Include *.hlsl, *.hlsli | ForEach-Object {
    $bytes = [System.IO.File]::ReadAllBytes($_.FullName)
    $hasBom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
    if (-not $hasBom) {
        $text = [System.Text.Encoding]::UTF8.GetString($bytes)
        [System.IO.File]::WriteAllText($_.FullName, $text, $utf8Bom)
        $fixed++
        Write-Host "  added UTF-8 BOM: $($_.FullName.Substring($root.Length + 1))"
    }
}
if ($fixed -gt 0) { Write-Host "fix_shader_encoding: $fixed file(s) updated." }
