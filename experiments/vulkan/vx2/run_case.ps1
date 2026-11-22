# VX2: one run of vx2_viewport with its log in build/tmp/vulkan_backend/vx2/<Name>.txt.
# Run from the worktree root after the build of build/vx2.
#   .\experiments\vulkan\vx2\run_case.ps1 -Name drag_vulkan_nvidia -CaseArgs '--api=vulkan --case=drag --device=NVIDIA'
param(
  [Parameter(Mandatory = $true)][string]$Name,
  [Parameter(Mandatory = $true)][string]$CaseArgs
)
$root = Get-Location
$exe  = Join-Path $root 'build\vx2\vx2_viewport.exe'
$logs = Join-Path $root 'build\tmp\vulkan_backend\vx2'
New-Item -ItemType Directory -Force $logs | Out-Null
$env:PATH = "D:\misc\Qt\6.9.3\msvc2022_64\bin;D:\Projects\pu-erh_lab\vcpkg\installed\x64-windows\bin;$env:VULKAN_SDK\Bin;$env:PATH"
$env:QSG_INFO = '1'
$log = Join-Path $logs "$Name.txt"
$passArgs = @($CaseArgs -split '\s+' | Where-Object { $_ -ne '' })
& $exe @passArgs *> $log
$code = $LASTEXITCODE
Add-Content $log "exit $code"
Get-Content $log | Select-String -Pattern '^RESULT|^ERROR|VALIDATION ERROR|^exit' | Select-Object -First 30 | ForEach-Object { $_.Line }
