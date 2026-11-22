# VX3: one run of vx3_load with its log in build/tmp/vulkan_backend/vx3/<Name>.txt, after the GPU
# idle check (gpu_idle_guard.ps1). Run from the worktree root after the build of build/vx3.
#   .\experiments\vulkan\vx3\run_case.ps1 -Name kernels_nvidia -CaseArgs '--case=kernels --device=NVIDIA'
param(
  [Parameter(Mandatory = $true)][string]$Name,
  [Parameter(Mandatory = $true)][string]$CaseArgs
)
$root = Get-Location
$exe  = Join-Path $root 'build\vx3\vx3_load.exe'
$logs = Join-Path $root 'build\tmp\vulkan_backend\vx3'
New-Item -ItemType Directory -Force $logs | Out-Null
$log = Join-Path $logs "$Name.txt"
Set-Content $log "ARGS $CaseArgs"
& (Join-Path $PSScriptRoot 'gpu_idle_guard.ps1') -Log $log | Out-Null
if ($LASTEXITCODE -ne 0) {
  Write-Output "GPU busy: $Name not run"
  exit 1
}
$env:PATH = "D:\misc\Qt\6.9.3\msvc2022_64\bin;D:\Projects\pu-erh_lab\vcpkg\installed\x64-windows\bin;$env:VULKAN_SDK\Bin;$env:PATH"
# Steam Fossilize writes .foz files into the working directory (VX2).
Push-Location $logs
$passArgs = @($CaseArgs -split '\s+' | Where-Object { $_ -ne '' })
& $exe @passArgs *>> $log
$code = $LASTEXITCODE
Pop-Location
Add-Content $log "exit $code"
# Validity: no other GPU work right after the run, and a clean idle window (the reference second
# before the load in the same run: no frame period above 1.5 vsyncs).
& (Join-Path $PSScriptRoot 'gpu_idle_guard.ps1') -Log $log -Once | Out-Null
$after = $LASTEXITCODE
$idle = Get-Content $log | Select-String -Pattern 'window=idle seconds=.* periods_over_1.5_vsync=(\d+)'
$idleBad = $idle -and [int]$idle.Matches[0].Groups[1].Value -gt 1
if ($after -ne 0 -or $idleBad) {
  Add-Content $log "VALIDITY invalid (other GPU work after the run: $($after -ne 0); idle window not clean: $idleBad)"
} else {
  Add-Content $log "VALIDITY valid"
}
Get-Content $log | Select-String -Pattern '^RESULT|^ERROR|VALIDATION ERROR|^exit|^GUARD|^SPIN|^VALIDITY' | Select-Object -First 60 | ForEach-Object { $_.Line }
