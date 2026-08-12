#requires -Version 5.1
<#
.SYNOPSIS
    Build Windows installer packages (WiX MSI / NSIS EXE / ZIP) for Alcedo Studio.
.DESCRIPTION
    This script automates the CMake install + CPack workflow on Windows.
    It detects available packaging tools and prints installation hints if they are missing.
    Build numbers increment after each successful package. Pass -BuildNumber to override.
    Run from the repository root.
.EXAMPLE
    .\scripts\package_windows.ps1 -BuildDir build\release -Preset win_release
#>
param(
    [string]$BuildDir = "$PSScriptRoot\..\build\release",
    [string]$Preset = "win_release",
    [string]$QtPrefix = "D:/Qt/6.9.3/msvc2022_64/lib/cmake",
    [string]$PackageOutDir = "$PSScriptRoot\..\build\release\package",
    [string]$DuckDbVssExtension = $env:ALCEDO_DUCKDB_VSS_EXTENSION,
    [string]$DuckDbFtsExtension = $env:ALCEDO_DUCKDB_FTS_EXTENSION,
    [bool]$RequireOpenCLAssets = $true,
    [ValidateRange(0, [long]::MaxValue)][long]$BuildNumber = 0,
    [ValidateSet('stable','beta')][string]$Channel = 'stable'
)

$ErrorActionPreference = 'Stop'
$repoRoot = Resolve-Path "$PSScriptRoot\.."
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
$PackageOutDir = [IO.Path]::GetFullPath($PackageOutDir)

function Invoke-BuildNumberState {
    param([ValidateSet('resolve','commit')][string]$Mode)

    $stateScript = Join-Path $repoRoot "scripts\update\build_number_state.cmake"
    $cmakeArgs = @(
        "-DALCEDO_BUILD_NUMBER_MODE=$Mode",
        "-DALCEDO_REPO_ROOT=$($repoRoot.Path.Replace('\', '/'))",
        "-DALCEDO_BUILD_DIR=$($BuildDir.Replace('\', '/'))",
        "-DALCEDO_BUILD_PLATFORM=windows",
        "-DALCEDO_BUILD_NUMBER_OVERRIDE=$BuildNumber",
        '-P',
        $stateScript
    )
    & (Join-Path $repoRoot "scripts\msvc_env.cmd") @cmakeArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to $Mode the Windows package build number."
    }
}

Invoke-BuildNumberState -Mode resolve
$pendingBuildNumberFile = Join-Path $repoRoot "build\tmp\update-build-number\windows.pending.txt"
$resolvedBuildNumberText = (Get-Content -LiteralPath $pendingBuildNumberFile -Raw).Trim()
$resolvedBuildNumber = 0L
if (-not [long]::TryParse($resolvedBuildNumberText, [ref]$resolvedBuildNumber) -or
    $resolvedBuildNumber -lt 1) {
    throw "Resolved Windows package build number is invalid: '$resolvedBuildNumberText'"
}

function Test-CommandAvailable {
    param([string]$Name)
    return ($null -ne (Get-Command $Name -ErrorAction SilentlyContinue))
}

function Resolve-DuckDbExtension {
    param(
        [Parameter(Mandatory = $true)][string]$ExtensionName,
        [Parameter(Mandatory = $true)][string]$FileName,
        [string]$ConfiguredPath,
        [string]$FallbackPath
    )

    if (-not [string]::IsNullOrWhiteSpace($ConfiguredPath)) {
        if (-not (Test-Path -LiteralPath $ConfiguredPath -PathType Leaf)) {
            throw "$ExtensionName DuckDB extension path does not exist: $ConfiguredPath"
        }
        return (Resolve-Path -LiteralPath $ConfiguredPath).Path
    }

    if (-not [string]::IsNullOrWhiteSpace($FallbackPath) -and
        (Test-Path -LiteralPath $FallbackPath -PathType Leaf)) {
        return (Resolve-Path -LiteralPath $FallbackPath).Path
    }

    if (-not (Test-CommandAvailable "duckdb")) {
        throw "DuckDB CLI is required to prepare the $ExtensionName extension. Install DuckDB or pass -DuckDb$($ExtensionName.Substring(0,1).ToUpper())$($ExtensionName.Substring(1))Extension."
    }

    $extensionRoot = Join-Path $BuildDir "duckdb_extensions"
    New-Item -ItemType Directory -Force -Path $extensionRoot | Out-Null

    $sqlExtensionRoot = $extensionRoot.Replace("'", "''")
    & duckdb -c "SET extension_directory='$sqlExtensionRoot'; INSTALL $ExtensionName;" | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to install DuckDB $ExtensionName extension."
    }

    $installed = Get-ChildItem -Path $extensionRoot -Filter $FileName -Recurse -File |
        Select-Object -First 1
    if (-not $installed) {
        throw "Failed to locate installed $FileName under $extensionRoot."
    }

    return $installed.FullName
}

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Alcedo Studio Windows Packager" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

$repoDuckDbExtensionDir = Join-Path $repoRoot "alcedo_studio\third_party\libduckdb-windows\extensions"
$repoVssExtension = Join-Path $repoDuckDbExtensionDir "vss.duckdb_extension"
$repoFtsExtension = Join-Path $repoDuckDbExtensionDir "fts.duckdb_extension"
$resolvedDuckDbVssExtension = Resolve-DuckDbExtension `
    -ExtensionName "vss" `
    -FileName "vss.duckdb_extension" `
    -ConfiguredPath $DuckDbVssExtension `
    -FallbackPath $repoVssExtension
$resolvedDuckDbFtsExtension = Resolve-DuckDbExtension `
    -ExtensionName "fts" `
    -FileName "fts.duckdb_extension" `
    -ConfiguredPath $DuckDbFtsExtension `
    -FallbackPath $repoFtsExtension

Write-Host "DuckDB VSS extension: $resolvedDuckDbVssExtension" -ForegroundColor Gray
Write-Host "DuckDB FTS extension: $resolvedDuckDbFtsExtension" -ForegroundColor Gray
Write-Host "Update build number: $resolvedBuildNumber" -ForegroundColor Gray
Write-Host ""

# ------------------------------------------------------------------
# 1. Configure (re-run to pick up new packaging tools like WiX/NSIS)
# ------------------------------------------------------------------
Write-Host "Configuring CMake with preset '$Preset' ..." -ForegroundColor Yellow
$configureCmd = "cmd /c `"$repoRoot\scripts\msvc_env.cmd`" --preset $Preset -DCMAKE_PREFIX_PATH=`"$QtPrefix`" -DALCEDO_DUCKDB_VSS_EXTENSION=`"$resolvedDuckDbVssExtension`" -DALCEDO_DUCKDB_FTS_EXTENSION=`"$resolvedDuckDbFtsExtension`" -DALCEDO_UPDATE_ALLOW_INSTALL=ON -DALCEDO_UPDATE_CHANNEL=$Channel -DALCEDO_BUILD_NUMBER=$resolvedBuildNumber"
Write-Host "> $configureCmd"
Invoke-Expression $configureCmd
if ($LASTEXITCODE -ne 0) {
    throw "CMake configuration failed."
}

# ------------------------------------------------------------------
# 2. Build install target
# ------------------------------------------------------------------
Write-Host "Building install target ..." -ForegroundColor Yellow
$buildCmd = "cmd /c `"$repoRoot\scripts\msvc_env.cmd`" --build $BuildDir --target install --parallel 4"
Write-Host "> $buildCmd"
Invoke-Expression $buildCmd
if ($LASTEXITCODE -ne 0) {
    throw "Build/install failed."
}

# ------------------------------------------------------------------
# 3. Verify install tree
# ------------------------------------------------------------------
Write-Host "Verifying install tree ..." -ForegroundColor Yellow
$verifyScript = Join-Path $repoRoot "scripts\verify_windows_install_tree.ps1"
$installDir = Join-Path $repoRoot "build\install"
$verifyArgs = @(
    '-ExecutionPolicy', 'Bypass',
    '-File', $verifyScript,
    '-InstallDir', $installDir
)
if (-not $RequireOpenCLAssets) {
    $verifyArgs += '-SkipOpenCLAssetCheck'
}
& powershell @verifyArgs
if ($LASTEXITCODE -ne 0) {
    throw "Install tree verification failed."
}

# ------------------------------------------------------------------
# 4. Run CPack
# ------------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $PackageOutDir | Out-Null
Write-Host "Running CPack ..." -ForegroundColor Yellow
$cpackCmd = "cpack --config `"$BuildDir\CPackConfig.cmake`" -B `"$PackageOutDir`""
Write-Host "> $cpackCmd"
Invoke-Expression $cpackCmd
if ($LASTEXITCODE -ne 0) {
    throw "CPack failed."
}

# ------------------------------------------------------------------
# 5. Report results
# ------------------------------------------------------------------
Write-Host ""
Write-Host "========================================" -ForegroundColor Green
Write-Host "  Packaging Complete" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green

$packages = Get-ChildItem -Path "$PackageOutDir\*" -Include *.msi,*.exe,*.zip
if ($packages) {
    foreach ($pkg in $packages) {
        $sizeMB = [math]::Round($pkg.Length / 1MB, 2)
        Write-Host "  Generated: $($pkg.Name) ($sizeMB MB)" -ForegroundColor Green
    }
} else {
    throw "No package files were generated in $PackageOutDir."
}

# Only consume the number after configure, build, install verification, CPack,
# and package discovery all succeeded. A failed run keeps the pending number so
# a retry rebuilds the same package identity.
Invoke-BuildNumberState -Mode commit

Write-Host ""

# ------------------------------------------------------------------
# 6. Tooling hints
# ------------------------------------------------------------------
$hasWix = (Test-CommandAvailable "candle.exe") -and (Test-CommandAvailable "light.exe")
$hasNsis = Test-CommandAvailable "makensis.exe"

if (-not $hasWix -and -not $hasNsis) {
    Write-Host "Notice: Neither WiX nor NSIS was detected. Only ZIP was generated." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "To generate a high-compression installer, install one of the following:" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "  WiX Toolset v3.11 (MSI):" -ForegroundColor White
    Write-Host "    https://github.com/wixtoolset/wix3/releases/tag/wix3112rtm"
    Write-Host "    Install and ensure candle.exe / light.exe are on PATH."
    Write-Host ""
    Write-Host "  NSIS (high-compression EXE):" -ForegroundColor White
    Write-Host "    https://nsis.sourceforge.io/Download"
    Write-Host "    Install and ensure makensis.exe is on PATH."
    Write-Host ""
    Write-Host "After installing, re-run this script to produce MSI or EXE installers." -ForegroundColor Cyan
} else {
    if ($hasWix) { Write-Host "WiX detected   : MSI package enabled" -ForegroundColor Green }
    if ($hasNsis) { Write-Host "NSIS detected  : EXE package enabled" -ForegroundColor Green }
}

Write-Host ""
