#requires -Version 5.1
param(
    [Parameter(Mandatory = $true)][string]$BinDir,
    [string]$CudaBinDir = "$env:CUDA_PATH\bin"
)
$ErrorActionPreference = 'Stop'
$cuobjdump = Join-Path $CudaBinDir 'cuobjdump.exe'
if (-not (Test-Path -LiteralPath $cuobjdump -PathType Leaf)) {
    throw "CUDA package inspection requires cuobjdump.exe: $cuobjdump"
}
$moduleList = Join-Path $BinDir 'cuda-runtime-modules.txt'
$modules = @(Get-Content -LiteralPath $moduleList | Where-Object { $_.Trim() })
if ($modules.Count -eq 0) { throw "No CUDA runtime modules in $moduleList" }
foreach ($module in $modules) {
    $modulePath = Join-Path $BinDir $module
    if (-not (Test-Path -LiteralPath $modulePath -PathType Leaf)) {
        throw "Missing CUDA runtime module: $modulePath"
    }
    $elf = & $cuobjdump --list-elf $modulePath | Out-String
    if ($LASTEXITCODE -ne 0) { throw "cuobjdump --list-elf failed: $modulePath" }
    $expectedCount = 0
    foreach ($architecture in @(60, 61, 70, 75, 80, 86, 89, 90)) {
        $count = [regex]::Matches($elf, "\.sm_$architecture\.cubin").Count
        if ($count -eq 0 -or ($expectedCount -ne 0 -and $count -ne $expectedCount)) {
            throw "CUDA module $module has $count objects for SM $architecture; expected the same nonzero count for each supported architecture ($expectedCount)."
        }
        $expectedCount = $count
    }
    $ptx = & $cuobjdump --list-ptx $modulePath | Out-String
    if ($LASTEXITCODE -ne 0) { throw "cuobjdump --list-ptx failed: $modulePath" }
    $ptxCount = [regex]::Matches($ptx, '\.sm_86\.ptx').Count
    # Linked and relocatable cubins can coexist. Verify kernel names below,
    # rather than assuming one PTX object for each native object.
    if ($ptxCount -eq 0) {
        throw "CUDA module $module has no compute 86 PTX."
    }
    $symbols = & $cuobjdump --dump-elf-symbols --gpu-architecture sm_86 $modulePath | Out-String
    if ($LASTEXITCODE -ne 0) { throw "Cannot read CUDA kernel symbols: $modulePath" }
    $ptxText = & $cuobjdump --dump-ptx --gpu-architecture sm_86 $modulePath | Out-String
    if ($LASTEXITCODE -ne 0) { throw "Cannot read CUDA PTX: $modulePath" }
    $entries = [regex]::Matches($symbols, '(?m)^STT_FUNC\s+STB_\w+\s+STO_ENTRY\s+(\S+)')
    # Some .cu modules contain host wrappers only (for example DemosaicNet).
    # They have no native kernel entry points to compare against PTX.
    $kernelNames = @($entries | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
    foreach ($name in $kernelNames) {
        if ($ptxText -cnotmatch "\.entry\s+$([regex]::Escape($name))\s*\(") {
            throw "CUDA kernel has native code but no compute 86 PTX in ${module}: $name"
        }
    }
    Write-Host "[alcedo] CUDA module ${module}: $expectedCount objects per native architecture, $ptxCount PTX objects, $($kernelNames.Count) kernel entries with PTX."
}
