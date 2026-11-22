# VX3: a series of runs. Screen 0 is the LG ULTRAFINE (NVIDIA output, 60 Hz), screen 1 the laptop
# panel (Intel output, 240 Hz) on the development computer (VX2). Run from the worktree root.
#   .\experiments\vulkan\vx3\run_series.ps1 -Series window_nvidia
# The export is the 45 megapixel render. The preview is the same render at 2560 pixels on the long
# edge, one request each frame (drag), or at 3840 pixels each 250 ms (settle).
param([Parameter(Mandatory = $true)][string]$Series, [string]$Suffix = '')
$run = Join-Path $PSScriptRoot 'run_case.ps1'
function WindowCases([string]$tag, [string]$dev, [int]$repeat) {
  $list = @()
  foreach ($p in @(@('drag', '--preview=2560 --preview-interval-ms=16.7'),
                   @('settle', '--preview=3840 --preview-interval-ms=250'))) {
    $pv = $p[1]
    $list += , @("win_$($tag)_$($p[0])_idle", "--case=idle $pv $dev")
    $list += , @("win_$($tag)_$($p[0])_whole", "--case=render --split=whole --repeat=$repeat $pv $dev")
    $list += , @("win_$($tag)_$($p[0])_budget16_if1", "--case=render --split=budget --budget-ms=16 --inflight=1 --repeat=$repeat $pv $dev")
    $list += , @("win_$($tag)_$($p[0])_budget8_if1", "--case=render --split=budget --budget-ms=8 --inflight=1 --repeat=$repeat $pv $dev")
    $list += , @("win_$($tag)_$($p[0])_budget4_if2", "--case=render --split=budget --budget-ms=4 --inflight=2 --repeat=$repeat $pv $dev")
  }
  $list += , @("win_$($tag)_drag_op", "--case=render --split=op --repeat=$repeat --preview=2560 --preview-interval-ms=16.7 $dev")
  return $list
}
$nv = '--device=NVIDIA --screen=0'
$in = '--device=Intel --screen=1'
$cases = @()
switch ($Series) {
  'window_nvidia' { $cases = WindowCases 'nvidia' $nv 12 }
  'window_intel' { $cases = WindowCases 'intel' $in 2 }
  'spin' {
    $pv = '--preview=2560 --preview-interval-ms=16.7'
    $cases = @(
      @('spin_nvidia_many', "--case=spin --spin=many --spin-ms=3000 --spin-count=300 $pv $nv"),
      @('spin_nvidia_wide', "--case=spin --spin=wide --spin-ms=3000 $pv $nv"),
      @('spin_nvidia_long', "--case=spin --spin=long --spin-ms=3000 $pv $nv"),
      @('spin_intel_many', "--case=spin --spin=many --spin-ms=3000 --spin-count=300 $pv $in"),
      @('spin_intel_wide', "--case=spin --spin=wide --spin-ms=3000 $pv $in"),
      @('spin_intel_long', "--case=spin --spin=long --spin-ms=3000 $pv $in")
    )
  }
}
# An invalid run (run_case.ps1, VALIDITY) runs again, up to 3 tries; each try keeps its own log.
$logs = Join-Path (Get-Location) 'build\tmp\vulkan_backend\vx3'
foreach ($c in $cases) {
  for ($try = 1; $try -le 3; $try++) {
    $name = if ($try -eq 1) { "$($c[0])$Suffix" } else { "$($c[0])$Suffix`_try$try" }
    Write-Output "=== $name"
    & $run -Name $name -CaseArgs $c[1] | Select-String -Pattern 'window=|preview=|run=0 |case=|^GUARD|^ERROR|exit|plan|device_lost=1|VALIDITY' | ForEach-Object { $_.Line }
    if (Select-String -Path (Join-Path $logs "$name.txt") -Pattern '^VALIDITY valid' -Quiet) { break }
  }
}
