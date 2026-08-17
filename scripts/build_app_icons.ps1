# Generates Windows/macOS app icons from the brand master using `cargo tauri icon`.
#
# Source: alcedo_studio/src/config/ICON/alcedo_icon.png
# Outputs:
#   - alcedo_studio/src/config/ICON/alcedo_icon.ico  (EXE resource via alcedo_main.rc)
#   - alcedo_studio/src/config/ICON/alcedo_icon.icns (macOS Dock/Finder/DMG native ICNS)
#   - packaging/windows/alcedo.ico                   (NSIS/WiX installer)
#   - packaging/macos/alcedo.icns                    (DMG/package icon copy)
#
# The 1024 PNG master is left in place. macOS Dock/Finder keep the native ICNS;
# this script never substitutes a PNG for the ICNS bundle/package icon.
#
# Prerequisites: cargo + tauri-cli (`cargo install tauri-cli --version "^2"`)
# Usage (repo root):
#   powershell -ExecutionPolicy Bypass -File scripts/build_app_icons.ps1

param(
    [string]$InputPng = "",
    [string]$IconDir = "$PSScriptRoot/../alcedo_studio/src/config/ICON",
    [string]$PackagingIco = "$PSScriptRoot/../packaging/windows/alcedo.ico",
    [string]$PackagingIcns = "$PSScriptRoot/../packaging/macos/alcedo.icns",
    [string]$WorkDir = "$PSScriptRoot/../build/tmp/alcedo_icons"
)

$ErrorActionPreference = 'Stop'

function Resolve-RepoPath {
    param([string]$Path)
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path $Path))
}

$repoRoot = Resolve-RepoPath (Join-Path $PSScriptRoot '..')
Set-Location $repoRoot

$IconDir = Resolve-RepoPath $IconDir
$PackagingIco = Resolve-RepoPath $PackagingIco
$PackagingIcns = Resolve-RepoPath $PackagingIcns
$WorkDir = Resolve-RepoPath $WorkDir

if ([string]::IsNullOrWhiteSpace($InputPng)) {
    $InputPng = Join-Path $IconDir 'alcedo_icon.png'
} else {
    $InputPng = Resolve-RepoPath $InputPng
}

if (-not (Test-Path -LiteralPath $InputPng)) {
    throw "Input PNG not found: $InputPng"
}

$tauri = Get-Command cargo-tauri -ErrorAction SilentlyContinue
if (-not $tauri) {
    $cargoTauri = Join-Path $env:USERPROFILE '.cargo\bin\cargo-tauri.exe'
    if (Test-Path -LiteralPath $cargoTauri) {
        $tauriPath = $cargoTauri
    } else {
        throw @"
tauri CLI not found. Install with:
  cargo install tauri-cli --version "^2"
Then re-run this script.
"@
    }
} else {
    $tauriPath = $tauri.Source
}

if (Test-Path -LiteralPath $WorkDir) {
    Remove-Item -LiteralPath $WorkDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
New-Item -ItemType Directory -Force -Path $IconDir | Out-Null
foreach ($pkgPath in @($PackagingIco, $PackagingIcns)) {
    $packagingDir = Split-Path -Parent $pkgPath
    if (-not [string]::IsNullOrWhiteSpace($packagingDir)) {
        New-Item -ItemType Directory -Force -Path $packagingDir | Out-Null
    }
}

Write-Output "Source : $InputPng"
Write-Output "Work   : $WorkDir"
Write-Output "Tauri  : $tauriPath"

& cargo tauri icon $InputPng -o $WorkDir
if ($LASTEXITCODE -ne 0) {
    throw "cargo tauri icon failed with exit code $LASTEXITCODE"
}

$generatedIco = Join-Path $WorkDir 'icon.ico'
$generatedIcns = Join-Path $WorkDir 'icon.icns'
$generatedPng = Join-Path $WorkDir 'icon.png'

foreach ($required in @($generatedIco, $generatedIcns, $generatedPng)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "tauri icon did not produce required file: $required"
    }
}

# Keep the designer-provided 1024 PNG master. tauri's icon.png is a
# downscaled preview and must not replace alcedo_icon.png, and the
# committed ICNS remains the macOS Dock/Finder/DMG icon.
$outPng = Join-Path $IconDir 'alcedo_icon.png'
if (-not (Test-Path -LiteralPath $outPng)) {
    Copy-Item -LiteralPath $generatedPng -Destination $outPng -Force
}

Copy-Item -LiteralPath $generatedIco -Destination (Join-Path $IconDir 'alcedo_icon.ico') -Force
Copy-Item -LiteralPath $generatedIcns -Destination (Join-Path $IconDir 'alcedo_icon.icns') -Force
Copy-Item -LiteralPath $generatedIco -Destination $PackagingIco -Force
Copy-Item -LiteralPath $generatedIcns -Destination $PackagingIcns -Force

# Optional desktop-size previews next to sources (not packaged).
$previewDir = Join-Path $IconDir 'generated'
New-Item -ItemType Directory -Force -Path $previewDir | Out-Null
foreach ($name in @(
    '32x32.png',
    '64x64.png',
    '128x128.png',
    'icon.png',
    'Square44x44Logo.png',
    'Square150x150Logo.png',
    'StoreLogo.png'
)) {
    $p = Join-Path $WorkDir $name
    if (Test-Path -LiteralPath $p) {
        Copy-Item -LiteralPath $p -Destination (Join-Path $previewDir $name) -Force
    }
}

function Get-IcoSummary {
    param([string]$Path)
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 6) { return 'invalid' }
    $count = [BitConverter]::ToUInt16($bytes, 4)
    $sizes = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $count; $i++) {
        $o = 6 + ($i * 16)
        $w = $bytes[$o]
        if ($w -eq 0) { $w = 256 }
        $sizes.Add([string]$w)
    }
    return ("$count sizes: " + ($sizes -join ', '))
}

$outIco = Join-Path $IconDir 'alcedo_icon.ico'
$outPng = Join-Path $IconDir 'alcedo_icon.png'
$outIcns = Join-Path $IconDir 'alcedo_icon.icns'

Write-Output ""
Write-Output "Wrote:"
Write-Output "  PNG  $outPng  ($((Get-Item $outPng).Length) bytes)"
Write-Output "  ICO  $outIco  ($((Get-Item $outIco).Length) bytes)  [$(Get-IcoSummary $outIco)]"
Write-Output "  ICNS $outIcns ($((Get-Item $outIcns).Length) bytes)"
Write-Output "  PKG  $PackagingIco ($((Get-Item $PackagingIco).Length) bytes)"
Write-Output "  PKG  $PackagingIcns ($((Get-Item $PackagingIcns).Length) bytes)"
Write-Output ""
Write-Output "Wiring (already in tree):"
Write-Output "  Windows EXE  : alcedo_main.rc -> alcedo_icon.ico"
Write-Output "  Taskbar/UI   : main.cpp setWindowIcon (ICO on Win, PNG elsewhere) via resource.qrc"
Write-Output "  macOS Dock   : MACOSX_BUNDLE_ICON_FILE=alcedo_studio_ao.icns in Contents/Resources"
Write-Output "  macOS DMG    : CPACK_PACKAGE_ICON -> alcedo_icon.icns"
Write-Output "Done"
