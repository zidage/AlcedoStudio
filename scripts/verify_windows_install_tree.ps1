#requires -Version 5.1
<#
.SYNOPSIS
    Verify that the Windows install tree contains the runtime payload needed on a clean user PC.
#>
param(
    [string]$InstallDir = "$PSScriptRoot\..\build\install",
    [string]$CudaBinDir = "$env:CUDA_PATH\bin",
    [switch]$SkipOpenCLAssetCheck
)

$ErrorActionPreference = 'Stop'

$installRoot = Resolve-Path $InstallDir
$binDir = Join-Path $installRoot 'bin'

function Assert-File {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing required file: $Path"
    }
}

function Assert-Directory {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "Missing required directory: $Path"
    }
}

function Assert-AnyFile {
    param(
        [string]$Directory,
        [string]$Filter
    )
    if (-not (Test-Path -LiteralPath $Directory -PathType Container)) {
        throw "Missing required directory: $Directory"
    }
    $matches = Get-ChildItem -LiteralPath $Directory -Filter $Filter -File -ErrorAction SilentlyContinue
    if (-not $matches) {
        throw "Missing required file matching '$Filter' in: $Directory"
    }
}

Assert-Directory $binDir

# Bundled gRPC/RE2 development files (headers, static libs, CMake/pkg-config, roots.pem)
# belong to the AlcedoGrpcDevelopment install component and must not reach the package.
foreach ($developmentDir in @('include', 'lib', 'share')) {
    $developmentPath = Join-Path $installRoot $developmentDir
    if (Test-Path -LiteralPath $developmentPath) {
        throw "Development files are in the install tree (install only the Unspecified component): $developmentPath"
    }
}

$requiredFiles = @(
    'alcedo_main.exe',
    'alcedo-cli.exe',
    'alcedo_studio_ao.ico',
    'alcedo_mind.exe',
    'DirectML.dll',
    'aria2c.exe',
    'duckdb.dll',
    'duckdb_extensions\fts.duckdb_extension',
    'duckdb_extensions\vss.duckdb_extension',
    'Qt6Core.dll',
    'Qt6Gui.dll',
    'Qt6Network.dll',
    'Qt6Qml.dll',
    'Qt6Quick.dll',
    'Qt6Widgets.dll',
    'vcruntime140.dll',
    'vcruntime140_1.dll',
    'vcomp140.dll',
    'msvcp140.dll',
    'fonts\main_Inter.ttf',
    'fonts\main_NotoSans_zh.ttf',
    'lensfun.dll',
    'editruntimecuda.dll',
    'editruntimelens.dll',
    'editscope.dll',
    'config\icc\rec709_gamma22.icc',
    'config\lens_calib\lens_catalog.json',
    'config\nikon_lens\id_map.json',
    'config\models\bayer.safetensors',
    'config\models\xtrans.safetensors',
    'configs\prompts\image_analysis_system_prompts.json'
)

foreach ($file in $requiredFiles) {
    Assert-File (Join-Path $binDir $file)
}

function Assert-MinSize {
    param(
        [string]$Path,
        [long]$MinBytes
    )
    Assert-File $Path
    $size = (Get-Item -LiteralPath $Path).Length
    if ($size -lt $MinBytes) {
        throw "File too small ($size < $MinBytes bytes), likely incomplete: $Path"
    }
}

# Real bayer/xtrans safetensors are ~130–160 KiB; LFS pointers are ~130 bytes.
Assert-MinSize (Join-Path $binDir 'config\models\bayer.safetensors') 10240
Assert-MinSize (Join-Path $binDir 'config\models\xtrans.safetensors') 10240

Assert-AnyFile -Directory $binDir -Filter 'cudart64_*.dll'
& (Join-Path $PSScriptRoot 'verify_windows_cuda_modules.ps1') -BinDir $binDir -CudaBinDir $CudaBinDir

if (-not $SkipOpenCLAssetCheck) {
    # Alcedo first-party OpenCL runtime DLLs (SHARED when CUDA DLLs are enabled).
    Assert-File (Join-Path $binDir 'OpenClContext.dll')
    Assert-File (Join-Path $binDir 'OpenClProgramLibrary.dll')

    # Khronos ICD loader: usually already installed by the GPU driver. Prefer the
    # system copy when present; warn (do not fail) if the package also omitted it.
    $bundledOpenClIcd = Join-Path $binDir 'OpenCL.dll'
    $systemOpenClIcd = Join-Path $env:SystemRoot 'System32\OpenCL.dll'
    if (-not (Test-Path -LiteralPath $bundledOpenClIcd -PathType Leaf) -and
        -not (Test-Path -LiteralPath $systemOpenClIcd -PathType Leaf)) {
        Write-Host "[alcedo] Warning: OpenCL.dll (Khronos ICD) not found in install tree or System32. OpenCL backends need a GPU driver ICD or a bundled loader." -ForegroundColor Yellow
    }

    $gpuCheck = Join-Path $binDir 'alcedo-gpu-check.exe'
    Assert-File $gpuCheck
    $savedGpuPath = $env:Path
    try {
        $env:Path = "$env:SystemRoot\System32;$env:SystemRoot"
        & $gpuCheck --opencl
        if ($LASTEXITCODE -ne 0) { throw 'Installed OpenCL program compilation failed.' }
    } finally {
        $env:Path = $savedGpuPath
    }
    # The legacy edit_pipeline OpenCL programs are archived (G10.10) and must not be packaged.
    $legacyOpenClDir = Join-Path $binDir 'opencl\edit\pipeline\opencl_shader'
    if (Test-Path -LiteralPath $legacyOpenClDir) {
        throw "Legacy OpenCL edit_pipeline shaders are packaged: $legacyOpenClDir"
    }
}

$exeIconScript = Join-Path $PSScriptRoot 'verify_windows_exe_icon.ps1'
$committedIco = (Resolve-Path -LiteralPath (
    Join-Path $PSScriptRoot '..\alcedo_studio\src\config\ICON\alcedo_icon.ico'
)).Path
& $exeIconScript -Exe (Join-Path $binDir 'alcedo_main.exe') -Ico $committedIco
if ($LASTEXITCODE -ne 0) {
    throw "Windows EXE PE icon does not match the committed ICO."
}

function Assert-FileContains {
    param(
        [string]$Path,
        [string]$Needle
    )
    Assert-File $Path
    $text = Get-Content -LiteralPath $Path -Raw -Encoding UTF8
    if ($null -eq $text -or $text.IndexOf($Needle) -lt 0) {
        throw "Required notice text '$Needle' is missing from: $Path"
    }
}

$noticeDir = Join-Path $binDir 'third_party_licenses'
Assert-File (Join-Path $binDir 'LICENSE')
Assert-File (Join-Path $binDir 'NOTICE')
Assert-File (Join-Path $binDir 'THIRD_PARTY_NOTICE.txt')
Assert-Directory $noticeDir
Assert-FileContains -Path (Join-Path $binDir 'NOTICE') -Needle 'QuickQanava'
Assert-FileContains -Path (Join-Path $binDir 'THIRD_PARTY_NOTICE.txt') -Needle 'QuickQanava'
Assert-FileContains -Path (Join-Path $binDir 'THIRD_PARTY_NOTICE.txt') -Needle 'bezier'
Assert-FileContains `
    -Path (Join-Path $noticeDir 'QuickQanava-licence.txt') `
    -Needle 'Redistributions in binary form must reproduce'
Assert-FileContains `
    -Path (Join-Path $noticeDir 'QuickQanava-bezier-LICENSE.txt') `
    -Needle 'MIT License'
Assert-FileContains `
    -Path (Join-Path $noticeDir 'QuickQanava-bezier-LICENSE.txt') `
    -Needle 'Permission is hereby granted'

$mainExe = Join-Path $binDir 'alcedo_main.exe'
$qmlStdoutPath = Join-Path $installRoot 'qml_import_probe_stdout.txt'
$qmlStderrPath = Join-Path $installRoot 'qml_import_probe_stderr.txt'
$qmlProbe = Start-Process -FilePath $mainExe -ArgumentList '--verify-qml-imports' `
    -WorkingDirectory $binDir -Wait -PassThru -NoNewWindow `
    -RedirectStandardOutput $qmlStdoutPath `
    -RedirectStandardError $qmlStderrPath
$qmlStdout = Get-Content -LiteralPath $qmlStdoutPath -Raw -ErrorAction SilentlyContinue
$qmlStderr = Get-Content -LiteralPath $qmlStderrPath -Raw -ErrorAction SilentlyContinue
$qmlOutput = "$qmlStdout`n$qmlStderr"
if ($qmlProbe.ExitCode -ne 0) {
    throw "Installed alcedo_main.exe --verify-qml-imports exited $($qmlProbe.ExitCode): $qmlOutput"
}
if ($qmlOutput -notmatch 'qml\.imports\.ok=QuickQanava') {
    throw "Installed alcedo_main.exe did not report a QuickQanava QML import: $qmlOutput"
}

# alcedo-cli must run with only the Windows directories on PATH, so that it and alcedo_main.exe
# load their DLLs from the install tree. `session start` finds alcedo_main.exe beside alcedo-cli.
$cliExe = Join-Path $binDir 'alcedo-cli.exe'
$cliProbeDir = Join-Path ([System.IO.Path]::GetTempPath()) "alcedo_cli_probe_$PID"
$cliSessionDir = Join-Path $cliProbeDir 'sessions'
$cliSettingsDir = Join-Path $cliProbeDir 'settings'
$cliProjectDir = Join-Path $cliProbeDir 'project'
New-Item -ItemType Directory -Force -Path $cliProjectDir | Out-Null
$cliSessionArgs = @('--session-dir', $cliSessionDir, '--session', 'install_probe')
$savedPath = $env:Path
# Windows PowerShell 5.1 turns stderr lines of a native program into errors; read them as text.
$savedErrorAction = $ErrorActionPreference
try {
    $ErrorActionPreference = 'Continue'
    $env:Path = "$env:SystemRoot\System32;$env:SystemRoot"
    $cliHelp = & $cliExe help 2>&1 | Out-String
    $cliHelpExit = $LASTEXITCODE
    $cliStart = & $cliExe @cliSessionArgs session start --headless `
        --create "$cliProjectDir,install_probe" --settings-dir $cliSettingsDir 2>&1 | Out-String
    $cliStartExit = $LASTEXITCODE
    $cliStop = ''
    $cliStopExit = 0
    if ($cliStartExit -eq 0) {
        $cliStop = & $cliExe @cliSessionArgs session stop 2>&1 | Out-String
        $cliStopExit = $LASTEXITCODE
    }
} finally {
    $env:Path = $savedPath
    $ErrorActionPreference = $savedErrorAction
}
if ($cliHelpExit -ne 0 -or $cliHelp -notmatch 'Usage: alcedo-cli') {
    throw "Installed alcedo-cli.exe help exited $cliHelpExit with a clean PATH: $cliHelp"
}
if ($cliStartExit -ne 0) {
    throw "Installed alcedo-cli.exe could not start a headless session (exit $cliStartExit): $cliStart"
}
if ($cliStopExit -ne 0) {
    throw "Installed alcedo-cli.exe could not stop the headless session (exit $cliStopExit): $cliStop"
}
Remove-Item -LiteralPath $cliProbeDir -Recurse -Force -ErrorAction SilentlyContinue

Write-Host "[alcedo] Windows install tree verification passed: $binDir" -ForegroundColor Green
