# VX5: a series of runs. Screen 0 is the LG ULTRAFINE (NVIDIA output, 60 Hz), screen 1 the laptop
# panel (Intel output, 240 Hz) on the development computer (VX2). Run from the worktree root.
#   .\experiments\vulkan\vx5\run_series.ps1 -Series budget_nvidia
param([Parameter(Mandatory = $true)][string]$Series, [string]$Suffix = '')
$run = Join-Path $PSScriptRoot 'run_case.ps1'
$nv  = '--device=NVIDIA'
$in  = '--device=Intel'
switch ($Series) {
  'budget_nvidia' {
    & $run -Name "budget_nvidia_none$Suffix" -CaseArgs "--case=budget $nv --priority=none" -TimeoutSeconds 300
    & $run -Name "budget_nvidia_split$Suffix" -CaseArgs "--case=budget $nv --priority=split" -TimeoutSeconds 300
    & $run -Name "budget_nvidia_pageable$Suffix" -CaseArgs "--case=budget $nv --priority=split --pageable=on" -TimeoutSeconds 300
    & $run -Name "budget_nvidia_other3g$Suffix" -CaseArgs "--case=budget $nv --priority=none" -Hold 3072 -TimeoutSeconds 300
  }
  'budget_intel' {
    # Iris Xe: the heap is system memory; the run stays below the budget (2 GiB).
    & $run -Name "budget_intel_2g$Suffix" -CaseArgs "--case=budget $in --max-mib=2048" -TimeoutSeconds 300
  }
  'tdr_nvidia' {
    # A GPU reset of Windows (dxcap -forcetdr) while Qt Quick draws on the NVIDIA display device.
    & $run -Name "tdr_nvidia_recreate$Suffix" -CaseArgs "--case=tdr $nv --screen=0 --display-restore=recreate" -TimeoutSeconds 180
  }
  'tdr_nvidia_reuse' {
    & $run -Name "tdr_nvidia_reuse$Suffix" -CaseArgs "--case=tdr $nv --screen=0 --display-restore=reuse" -TimeoutSeconds 180
  }
  default { Write-Output "unknown series $Series" }
}
