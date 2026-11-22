#requires -Version 5.1
param(
    [string]$CopyScript = "$PSScriptRoot\..\cmake\copy_linked_runtime_dlls.cmake",
    [string]$WorkDir = "$PSScriptRoot\..\..\build\tmp\linked-runtime-dll-copy"
)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path "$PSScriptRoot\..\..").Path
$WorkDir = [IO.Path]::GetFullPath($WorkDir)
if (-not $WorkDir.StartsWith((Join-Path $repoRoot 'build\tmp\'), [StringComparison]::OrdinalIgnoreCase) -or
    (Test-Path -LiteralPath $WorkDir)) {
    throw 'Use a new test directory under build/tmp/.'
}
$runtimeDir = Join-Path $WorkDir 'alcedo_studio\src'
$productDir = Join-Path $runtimeDir 'opencl'
$unrelatedDir = Join-Path $runtimeDir 'decoders'
New-Item -ItemType Directory -Force -Path $productDir, $unrelatedDir | Out-Null
[IO.File]::WriteAllText((Join-Path $WorkDir 'CMakeCache.txt'), '')
$linked = Join-Path $productDir 'Linked.dll'
$unrelated = Join-Path $unrelatedDir 'Unrelated.dll'
$installedLinked = Join-Path $runtimeDir 'Linked.dll'
$installedUnrelated = Join-Path $runtimeDir 'Unrelated.dll'
[IO.File]::WriteAllText($linked, 'current linked output')
[IO.File]::WriteAllText($unrelated, 'product being linked')
[IO.File]::WriteAllText($installedLinked, 'stale linked output')
[IO.File]::WriteAllText($installedUnrelated, 'unrelated local copy')

# Match the production interleaving: the runtime refresh can run while an unrelated DLL
# is open exclusively by the Windows linker. Its own linked DLL is already complete.
$locked = [IO.File]::Open($unrelated, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite,
    [IO.FileShare]::None)
try {
    & "$repoRoot\scripts\msvc_env.cmd" "-DALCEDO_RUNTIME_DLLS=$linked" `
        "-DALCEDO_RUNTIME_DEST=$runtimeDir" -P $CopyScript
    if ($LASTEXITCODE -ne 0) { throw 'The refresh read an unrelated locked product DLL.' }
} finally {
    $locked.Dispose()
}
if ([IO.File]::ReadAllText($installedLinked) -ne 'current linked output') {
    throw 'The linked DLL copy is stale.'
}
if ([IO.File]::ReadAllText($installedUnrelated) -ne 'unrelated local copy') {
    throw 'The refresh changed an unrelated local DLL.'
}
Write-Host 'PASS LinkedDllRefreshCompletesWhileUnrelatedProductIsLocked'

$missing = Join-Path $productDir 'Missing.dll'
$failureLog = Join-Path $WorkDir 'missing-linked-dll.txt'
& "$repoRoot\scripts\msvc_env.cmd" "-DALCEDO_RUNTIME_DLLS=$missing" `
    "-DALCEDO_RUNTIME_DEST=$runtimeDir" -P $CopyScript *> $failureLog
if ($LASTEXITCODE -eq 0 -or (Get-Content -LiteralPath $failureLog -Raw) -notmatch
    'Linked runtime DLL does not exist') {
    throw 'A missing linked DLL did not report its real error.'
}
Write-Host 'PASS MissingLinkedDllReportsAnError'
