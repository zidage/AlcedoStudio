# VX1: the first spirv-opt pass of -O after which the Intel driver fails in vkCreateComputePipelines.
# Input: an -O0 module that creates its pipeline. Run from the worktree root.
param([string]$Module = "build\vx1\spv\primary_grade_curve_form0-O0.spv", [string]$Device = "Intel")
$bin = "$env:VULKAN_SDK\Bin"
$help = & "$bin\spirv-opt.exe" --help 2>&1
$start = ($help | Select-String -Pattern '^  -O$').LineNumber
$passes = @()
for ($i = $start; $i -lt $help.Count; ++$i) {
  $line = $help[$i]
  if ($line -match '^\t\t(\S+)$') { $passes += $Matches[1] }
  elseif ($passes.Count -gt 0 -and $line -notmatch '^\t\t') { break }
}
"passes of -O: $($passes.Count)"
function Test-Prefix([int]$count) {
  $flags = @(); if ($count -gt 0) { $flags = $passes[0..($count - 1)] | ForEach-Object { "--$_" } }
  & "$bin\spirv-opt.exe" --target-env=vulkan1.3 @flags $Module -o build\vx1\spv\bisect.spv 2>&1 | Out-Null
  & .\build\vx1\vx1_runner.exe --case pipeline --vk-device $Device --spv build\vx1\spv --grade-spv bisect.spv 2>&1 | Out-Null
  return $LASTEXITCODE -eq 0
}
if (-not (Test-Prefix 0)) { "the module alone fails: no pass is needed"; exit 1 }
$good = 0
$bad = $passes.Count
if (-not (Test-Prefix $bad)) {
  while ($bad - $good -gt 1) {
    $mid = [int](($good + $bad) / 2)
    if (Test-Prefix $mid) { $good = $mid } else { $bad = $mid }
  }
  "first failing prefix: $bad passes; the last pass is '$($passes[$bad - 1])'"
  "prefix: $($passes[0..($bad - 1)] -join ' ')"
} else {
  "every pass of -O creates the pipeline"
}
