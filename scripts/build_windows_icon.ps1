# Compatibility wrapper. Prefer scripts/build_app_icons.ps1 (tauri icon).
#
# Usage (repo root):
#   powershell -ExecutionPolicy Bypass -File scripts/build_windows_icon.ps1

param(
    [string]$InputPng = "",
    [string]$OutputIco = "",
    [int[]]$Sizes = @(),
    [double]$PaddingRatio = 0.04,
    [int]$AlphaThreshold = 8,
    [int]$MaskAlphaThreshold = 0
)

$ErrorActionPreference = 'Stop'

# Ignore legacy sizing parameters; tauri icon owns multi-size packing.
$legacyArgs = @{}
if (-not [string]::IsNullOrWhiteSpace($InputPng)) {
    $legacyArgs['InputPng'] = $InputPng
}

& "$PSScriptRoot/build_app_icons.ps1" @legacyArgs
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

if (-not [string]::IsNullOrWhiteSpace($OutputIco)) {
    $defaultIco = Join-Path $PSScriptRoot '../alcedo_studio/src/config/ICON/alcedo_icon.ico'
    $defaultIco = [System.IO.Path]::GetFullPath($defaultIco)
    $OutputIcoResolved = if ([System.IO.Path]::IsPathRooted($OutputIco)) {
        $OutputIco
    } else {
        [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path $OutputIco))
    }
    if ($OutputIcoResolved -ne $defaultIco) {
        $outDir = Split-Path -Parent $OutputIcoResolved
        if (-not [string]::IsNullOrWhiteSpace($outDir)) {
            New-Item -ItemType Directory -Force -Path $outDir | Out-Null
        }
        Copy-Item -LiteralPath $defaultIco -Destination $OutputIcoResolved -Force
        Write-Output "Also copied ICO to: $OutputIcoResolved"
    }
}
