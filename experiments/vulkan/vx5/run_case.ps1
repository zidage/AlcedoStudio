# VX5: one run of vx5_memory with its log in build/tmp/vulkan_backend/vx5/<Name>.txt, after the
# GPU idle check of VX3 (vx3/gpu_idle_guard.ps1). Run from the worktree root after the build of
# build/vx5.
#   .\experiments\vulkan\vx5\run_case.ps1 -Name budget_nvidia_none -CaseArgs '--case=budget --device=NVIDIA'
# -Hold: a second vx5_memory process (--case=hold) holds that many MiB during the run.
# A run that takes more than 90 s is stopped (a hang after a device loss).
param(
  [Parameter(Mandatory = $true)][string]$Name,
  [Parameter(Mandatory = $true)][string]$CaseArgs,
  [int]$Hold = 0,
  [string]$HoldDevice = 'NVIDIA',
  [int]$TimeoutSeconds = 90
)
$root = Get-Location
$exe  = Join-Path $root 'build\vx5\vx5_memory.exe'
$logs = Join-Path $root 'build\tmp\vulkan_backend\vx5'
New-Item -ItemType Directory -Force $logs | Out-Null
$log = Join-Path $logs "$Name.txt"
Set-Content $log "ARGS $CaseArgs HOLD $Hold"
& (Join-Path $PSScriptRoot '..\vx3\gpu_idle_guard.ps1') -Log $log | Out-Null
if ($LASTEXITCODE -ne 0) {
  Write-Output "GPU busy: $Name not run"
  exit 1
}
$env:PATH = "D:\misc\Qt\6.9.3\msvc2022_64\bin;D:\Projects\pu-erh_lab\vcpkg\installed\x64-windows\bin;$env:VULKAN_SDK\Bin;$env:PATH"
$env:QT_FORCE_STDERR_LOGGING = '1'
# Steam Fossilize writes .foz files into the working directory (VX2).
Push-Location $logs
$holder = $null
if ($Hold -gt 0) {
  $holdLog = Join-Path $logs "$Name.hold.txt"
  $holder = Start-Process -FilePath $exe -ArgumentList @('--case=hold', "--device=$HoldDevice", "--hold-mib=$Hold", '--hold-seconds=60') `
    -RedirectStandardOutput $holdLog -RedirectStandardError "$holdLog.err" -NoNewWindow -PassThru
  $deadline = (Get-Date).AddSeconds(30)
  while ((Get-Date) -lt $deadline -and -not (Select-String -Path $holdLog -Pattern 'hold_ready' -Quiet)) {
    Start-Sleep -Milliseconds 200
  }
  Add-Content $log "HOLDER pid=$($holder.Id) ready=$((Select-String -Path $holdLog -Pattern 'hold_ready' -Quiet))"
}
$out = Join-Path $logs "$Name.stdout.tmp"
$err = Join-Path $logs "$Name.stderr.tmp"
$passArgs = @($CaseArgs -split '\s+' | Where-Object { $_ -ne '' })
$p = Start-Process -FilePath $exe -ArgumentList $passArgs -RedirectStandardOutput $out -RedirectStandardError $err -NoNewWindow -PassThru
$done = $p.WaitForExit($TimeoutSeconds * 1000)
if (-not $done) {
  Stop-Process -Id $p.Id -Force
  Add-Content $log "KILLED after $TimeoutSeconds s"
  $p.WaitForExit()
}
$code = $p.ExitCode
if ($holder) {
  Stop-Process -Id $holder.Id -Force -ErrorAction SilentlyContinue
  Get-Content $holdLog | Select-String -Pattern '^RESULT' | ForEach-Object { Add-Content $log "HOLD $($_.Line)" }
}
Get-Content $out | Add-Content $log
Get-Content $err | ForEach-Object { Add-Content $log "STDERR $_" }
Remove-Item $out, $err -ErrorAction SilentlyContinue
Pop-Location
Add-Content $log "exit $code"
& (Join-Path $PSScriptRoot '..\vx3\gpu_idle_guard.ps1') -Log $log -Once | Out-Null
if ($LASTEXITCODE -ne 0) { Add-Content $log 'VALIDITY invalid (other GPU work after the run)' } else { Add-Content $log 'VALIDITY valid' }
Get-Content $log | Select-String -Pattern '^RESULT|ERROR|^exit|^GUARD|^VALIDITY|^KILLED|^QT|^WATCH|STDERR' | Select-Object -First 120 | ForEach-Object { $_.Line }
