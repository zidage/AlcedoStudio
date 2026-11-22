# VX3: wait until the GPUs have no other work before a timing run. Other work: a process of the
# model experiments (python, onnxruntime, alcedo programs, test programs), more than 10 % GPU
# engine use by a process other than the desktop compositor, or NVIDIA use above 40 % (the desktop
# at P8 clocks shows 15 to 40 %). The script waits; it stops nothing.
# Writes one line for each check to the log, and exits 1 after -MaxMinutes. -Once: one check, exit
# 1 when busy (the check after a run).
param([string]$Log = '', [int]$MaxMinutes = 120, [switch]$Once)
$pattern = 'python|onnx|alcedo|denoise|utnet|timingtool|_test$|ctest|torch'
$deadline = (Get-Date).AddMinutes($MaxMinutes)
$dwm = @(Get-Process dwm -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
function Note([string]$text) {
  $line = "GUARD $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $text"
  Write-Output $line
  if ($Log -ne '') { Add-Content $Log $line }
}
$quiet = 0
while ($true) {
  $procs = @(Get-Process | Where-Object { $_.ProcessName -match $pattern -and $_.ProcessName -ne 'vx3_load' })
  $nv = 0
  try { $nv = [int]((nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits) | Select-Object -First 1) } catch {}
  $busy = @()
  $samples = (Get-Counter '\GPU Engine(*)\Utilization Percentage' -ErrorAction SilentlyContinue).CounterSamples
  $byPid = @{}
  foreach ($s in $samples) {
    if ($s.InstanceName -match '^pid_(\d+)_') {
      $id = [int]$Matches[1]
      if ($id -eq 0 -or $dwm -contains $id) { continue }
      $byPid[$id] = [double]$byPid[$id] + $s.CookedValue
    }
  }
  foreach ($id in $byPid.Keys) {
    if ($byPid[$id] -gt 10) {
      $name = (Get-Process -Id $id -ErrorAction SilentlyContinue).ProcessName
      $busy += "$name($id)=$([math]::Round($byPid[$id]))%"
    }
  }
  $names = ($procs | ForEach-Object { "$($_.ProcessName)($($_.Id))" }) -join ','
  if ($procs.Count -eq 0 -and $busy.Count -eq 0 -and $nv -le 40) {
    $quiet++
    if ($quiet -ge 2 -or $Once) {
      Note "idle nvidia=$nv%"
      exit 0
    }
  } else {
    $quiet = 0
    Note "busy processes=[$names] engines=[$($busy -join ',')] nvidia=$nv% - waiting"
    if ($Once) { exit 1 }
  }
  if ((Get-Date) -gt $deadline) {
    Note "still busy after $MaxMinutes min - no run"
    exit 1
  }
  Start-Sleep -Seconds 20
}
