# VX2: a series of runs. Screen 0 is the LG ULTRAFINE (NVIDIA output, 60 Hz), screen 1 the laptop
# panel (Intel output, 240 Hz) on the development computer. Run from the worktree root.
#   .\experiments\vulkan\vx2\run_series.ps1 -Series timing
param([Parameter(Mandatory = $true)][string]$Series, [string]$Suffix = '')
$run = Join-Path $PSScriptRoot 'run_case.ps1'
$cases = @()
switch ($Series) {
  'timing' {
    $cases = @(
      @('drag_d3d11_nvidia_s0', '--api=d3d11 --case=drag --device=NVIDIA --screen=0'),
      @('drag_vulkan_nvidia_separate_s0', '--api=vulkan --case=drag --device=NVIDIA --screen=0'),
      @('drag_vulkan_nvidia_shared_frame_s0', '--api=vulkan --case=drag --device=NVIDIA --queue=shared --lock-span=frame --screen=0'),
      @('drag_vulkan_nvidia_shared_submit_s0', '--api=vulkan --case=drag --device=NVIDIA --queue=shared --lock-span=submit --screen=0'),
      @('drag_d3d11_intel_s1', '--api=d3d11 --case=drag --device=Intel --screen=1'),
      @('drag_vulkan_intel_shared_frame_s1', '--api=vulkan --case=drag --device=Intel --queue=shared --lock-span=frame --screen=1'),
      @('drag_vulkan_intel_shared_submit_s1', '--api=vulkan --case=drag --device=Intel --queue=shared --lock-span=submit --screen=1'),
      @('resize_d3d11_nvidia_s0', '--api=d3d11 --case=resize --device=NVIDIA --screen=0'),
      @('resize_vulkan_nvidia_separate_s0', '--api=vulkan --case=resize --device=NVIDIA --screen=0'),
      @('resize_d3d11_intel_s1', '--api=d3d11 --case=resize --device=Intel --screen=1'),
      @('resize_vulkan_intel_shared_submit_s1', '--api=vulkan --case=resize --device=Intel --queue=shared --lock-span=submit --screen=1')
    )
  }
  'cross' {
    $cases = @(
      @('drag_vulkan_nvidia_split_s1', '--api=vulkan --case=drag --device=NVIDIA --queue=split --screen=1'),
      @('drag_d3d11_nvidia_s1', '--api=d3d11 --case=drag --device=NVIDIA --screen=1'),
      @('drag_vulkan_intel_split_s0', '--api=vulkan --case=drag --device=Intel --queue=split --screen=0'),
      @('drag_d3d11_intel_s0', '--api=d3d11 --case=drag --device=Intel --screen=0'),
      @('drag_d3d11_default_s0', '--api=d3d11 --case=drag --screen=0'),
      @('drag_d3d11_default_s1', '--api=d3d11 --case=drag --screen=1')
    )
  }
  'isolate' {
    $cases = @(
      @('resize_vulkan_intel_shared_nomutex_s1', '--api=vulkan --case=resize --device=Intel --queue=shared --mutex=off --screen=1'),
      @('resize_vulkan_intel_shared_submit_s1_r2', '--api=vulkan --case=resize --device=Intel --queue=shared --lock-span=submit --screen=1'),
      @('resize_d3d11_intel_s1_r2', '--api=d3d11 --case=resize --device=Intel --screen=1'),
      @('resize_vulkan_nvidia_shared_s0', '--api=vulkan --case=resize --device=NVIDIA --queue=shared --lock-span=submit --screen=0'),
      @('resize_vulkan_nvidia_separate_s0_r2', '--api=vulkan --case=resize --device=NVIDIA --screen=0'),
      @('resize_d3d11_nvidia_s0_r2', '--api=d3d11 --case=resize --device=NVIDIA --screen=0'),
      @('resize_vulkan_nvidia_separate_s1', '--api=vulkan --case=resize --device=NVIDIA --screen=1'),
      @('resize_d3d11_nvidia_s1', '--api=d3d11 --case=resize --device=NVIDIA --screen=1'),
      @('drag_d3d11_intel_small_s1', '--api=d3d11 --case=drag --device=Intel --screen=1 --image=1280x800'),
      @('drag_vulkan_intel_small_s1', '--api=vulkan --case=drag --device=Intel --queue=shared --lock-span=submit --screen=1 --image=1280x800'),
      @('drag_vulkan_nvidia_separate_s0_r2', '--api=vulkan --case=drag --device=NVIDIA --screen=0'),
      @('drag_vulkan_nvidia_shared_submit_s0_r2', '--api=vulkan --case=drag --device=NVIDIA --queue=shared --lock-span=submit --screen=0')
    )
  }
  'split' {
    $o = 'build\tmp\vulkan_backend\vx2'
    $stress = '--case=stress --validation=on --produce=continuous --work=64 --seconds=10'
    $cases = @(
      @('grab_vulkan_intel_split_s0', "--api=vulkan --case=grab --device=Intel --queue=split --screen=0 --validation=on --out=$o\grab_vulkan_intel_split_s0.png"),
      @('grab_d3d11_intel_s0', "--api=d3d11 --case=grab --device=Intel --screen=0 --out=$o\grab_d3d11_intel_s0.png"),
      @('stress_nvidia_split', "--api=vulkan --device=NVIDIA --queue=split --screen=0 $stress"),
      @('stress_intel_split', "--api=vulkan --device=Intel --queue=split --screen=1 $stress"),
      @('drag_vulkan_nvidia_split_s0', '--api=vulkan --case=drag --device=NVIDIA --queue=split --screen=0'),
      @('drag_vulkan_intel_split_s1', '--api=vulkan --case=drag --device=Intel --queue=split --screen=1'),
      @('resize_vulkan_nvidia_split_s0', '--api=vulkan --case=resize --device=NVIDIA --queue=split --screen=0'),
      @('resize_vulkan_intel_split_s1', '--api=vulkan --case=resize --device=Intel --queue=split --screen=1'),
      @('heavy_vulkan_nvidia_split_s0', '--api=vulkan --case=drag --device=NVIDIA --queue=split --screen=0 --produce=continuous --work=4000'),
      @('heavy_vulkan_nvidia_separate_s0', '--api=vulkan --case=drag --device=NVIDIA --screen=0 --produce=continuous --work=4000'),
      @('heavy_vulkan_nvidia_shared_submit_s0', '--api=vulkan --case=drag --device=NVIDIA --queue=shared --lock-span=submit --screen=0 --produce=continuous --work=4000'),
      @('heavy_vulkan_intel_split_s1', '--api=vulkan --case=drag --device=Intel --queue=split --screen=1 --produce=continuous --work=400'),
      @('heavy_vulkan_intel_shared_frame_s1', '--api=vulkan --case=drag --device=Intel --queue=shared --lock-span=frame --screen=1 --produce=continuous --work=400'),
      @('heavy_vulkan_intel_shared_submit_s1', '--api=vulkan --case=drag --device=Intel --queue=shared --lock-span=submit --screen=1 --produce=continuous --work=400')
    )
  }
  'threads' {
    $common = '--case=stress --validation=on --produce=continuous --work=64 --seconds=10'
    $cases = @(
      @('stress_nvidia_separate', "--api=vulkan --device=NVIDIA --screen=0 $common"),
      @('stress_nvidia_shared_nomutex', "--api=vulkan --device=NVIDIA --queue=shared --mutex=off --screen=0 $common"),
      @('stress_nvidia_shared_frame', "--api=vulkan --device=NVIDIA --queue=shared --lock-span=frame --screen=0 $common"),
      @('stress_nvidia_shared_submit', "--api=vulkan --device=NVIDIA --queue=shared --lock-span=submit --screen=0 $common"),
      @('stress_intel_shared_nomutex', "--api=vulkan --device=Intel --queue=shared --mutex=off --screen=1 $common"),
      @('stress_intel_shared_frame', "--api=vulkan --device=Intel --queue=shared --lock-span=frame --screen=1 $common"),
      @('stress_intel_shared_submit', "--api=vulkan --device=Intel --queue=shared --lock-span=submit --screen=1 $common")
    )
  }
  default { throw "unknown series $Series" }
}
foreach ($c in $cases) {
  $name = $c[0] + $Suffix
  Write-Output "== $name"
  & $run -Name $name -CaseArgs $c[1] | Where-Object { $_ -match '^RESULT case=|period_ms_|gui_late|begin_frame|between_frames|content_latency|resize_latency|producer_|validation_errors|^ERROR|^exit' }
}
