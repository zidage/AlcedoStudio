#requires -Version 5.1
param(
    [string]$BuildDir = "$PSScriptRoot\..\..\build\debug",
    [string]$WorkDir = "$PSScriptRoot\..\..\build\tmp\gpu-package-checks"
)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path "$PSScriptRoot\..\..").Path
$WorkDir = [IO.Path]::GetFullPath($WorkDir)
if (-not $WorkDir.StartsWith((Join-Path $repoRoot 'build\tmp\'), [StringComparison]::OrdinalIgnoreCase)) {
    throw 'GPU package test files must be under build/tmp/.'
}
if (Test-Path -LiteralPath $WorkDir) { throw "Use a new test directory: $WorkDir" }
$binDir = Join-Path $WorkDir 'install\bin'
New-Item -ItemType Directory -Path $binDir -Force | Out-Null
$runtimeDir = Join-Path $BuildDir 'alcedo_studio\src'
Copy-Item -LiteralPath (Join-Path $runtimeDir 'alcedo-gpu-check.exe') -Destination $binDir
Get-ChildItem -LiteralPath $runtimeDir -Filter '*.dll' -File | Copy-Item -Destination $binDir
$inventoryScript = Join-Path $BuildDir 'install_gpu_checksums_bin_opencl.cmake'
$inventoryText = Get-Content -LiteralPath $inventoryScript -Raw
$sourceRoot = [regex]::Match($inventoryText, 'set\(_gpu_source_root "([^"]+)"\)').Groups[1].Value
$files = [regex]::Match($inventoryText, 'set\(_gpu_files "([^"]+)"\)').Groups[1].Value.Split(';')
if (-not $sourceRoot -or $files.Count -lt 2) { throw 'Cannot read generated GPU asset inventory.' }
foreach ($file in $files) {
    $relative = $file.Substring($sourceRoot.Length + 1)
    $destination = Join-Path $binDir "opencl\$relative"
    New-Item -ItemType Directory -Path (Split-Path $destination) -Force | Out-Null
    Copy-Item -LiteralPath $file -Destination $destination
}
function Write-Inventory {
    & "$repoRoot\scripts\msvc_env.cmd" "-DCMAKE_INSTALL_PREFIX=$WorkDir/install" -P $inventoryScript
    if ($LASTEXITCODE -ne 0) { throw 'Generating the test package inventory failed.' }
}
function Assert-Check {
    param([string]$Name, [bool]$Success, [string]$ExpectedText)
    $stdout = Join-Path $WorkDir "$Name.stdout.txt"
    $stderr = Join-Path $WorkDir "$Name.stderr.txt"
    $savedPath = $env:Path
    try {
        $env:Path = "$env:SystemRoot\System32;$env:SystemRoot"
        $process = Start-Process -FilePath (Join-Path $binDir 'alcedo-gpu-check.exe') `
            -ArgumentList '--opencl' -WorkingDirectory $WorkDir -WindowStyle Hidden -Wait -PassThru `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    } finally { $env:Path = $savedPath }
    $output = (Get-Content $stdout -Raw) + (Get-Content $stderr -Raw)
    if (($process.ExitCode -eq 0) -ne $Success -or -not $output.Contains($ExpectedText)) {
        throw "$Name failed (exit $($process.ExitCode)): $output"
    }
    Write-Host "PASS $Name"
    if ($Success) { Write-Host ($output -split "`n" | Where-Object { $_ -match 'gpu.kernels.ok' }) }
}
Write-Inventory
Assert-Check 'AllInstalledProgramsCompileAndCreateKernelsWithCleanPath' $true 'gpu.kernels.ok=opencl'

$relative = 'edit/runtime/opencl/shader/ai_mask_coverage.cl'
$source = Join-Path $sourceRoot $relative
$asset = Join-Path $binDir "opencl\$relative"
Remove-Item -LiteralPath $asset
Assert-Check 'MissingKernelSourceReportsPackagedPath' $false 'Missing GPU asset:'
Copy-Item -LiteralPath $source -Destination $asset

$shaderDir = Split-Path $asset
$removedDir = Join-Path $WorkDir 'removed-shaders'
# Both absolute paths are under the task-owned test directory checked above.
Move-Item -LiteralPath $shaderDir -Destination $removedDir
Assert-Check 'MissingShaderSubtreeDoesNotReadCheckout' $false 'Missing GPU asset:'
Move-Item -LiteralPath $removedDir -Destination $shaderDir

[IO.File]::AppendAllText($asset, "`n#error package_compile_failure`n", [Text.UTF8Encoding]::new($false))
Assert-Check 'ChangedSourceFailsChecksum' $false 'GPU asset differs from build input:'

$manifest = Join-Path $binDir 'opencl\runtime-assets.sha256'
$originalManifest = [IO.File]::ReadAllText($manifest).Replace("`r`n", "`n")
$hash = (Get-FileHash -LiteralPath $asset -Algorithm SHA256).Hash.ToLowerInvariant()
$updated = $originalManifest -replace "(?m)^[a-f0-9]{64}  $([regex]::Escape($relative))$", "$hash  $relative"
[IO.File]::WriteAllText($manifest, $updated, [Text.UTF8Encoding]::new($false))
Assert-Check 'CompilerFailureReportsProgramAndDriver' $false 'failed to build program'
$compilerError = Get-Content (Join-Path $WorkDir 'CompilerFailureReportsProgramAndDriver.stderr.txt') -Raw
if ($compilerError -notmatch "program='[^']+'" -or $compilerError -notmatch "driver='[^']+'" -or
    $compilerError -notmatch 'package_compile_failure') {
    throw "Compiler diagnostics omit the program, driver, or source error: $compilerError"
}
Copy-Item -LiteralPath $source -Destination $asset

$unlisted = ($originalManifest -split "`n" | Where-Object { -not $_.EndsWith("  $relative") }) -join "`n"
[IO.File]::WriteAllText($manifest, $unlisted, [Text.UTF8Encoding]::new($false))
Assert-Check 'UnlistedRegisteredSourceFailsPackageInventory' $false 'absent from the package inventory:'
Remove-Item -LiteralPath $manifest
Assert-Check 'MissingInventoryFailsBeforeGpuCompilation' $false 'Cannot read'
Write-Host 'GPU package verification: 7 checks passed.'
