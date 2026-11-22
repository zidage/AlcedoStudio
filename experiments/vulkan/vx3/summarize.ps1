# VX3: one line for each window run log: the reference second (idle p99), the Qt frames during the
# export, the preview render latency before and during the export, and the export time.
#   .\experiments\vulkan\vx3\summarize.ps1 -Filter 'win_nvidia_*'
param([string]$Filter = 'win_*')
$logs = Join-Path (Get-Location) 'build\tmp\vulkan_backend\vx3'
function Field([string]$text, [string]$pattern, [string]$name) {
  $line = ($text -split "`n") | Where-Object { $_ -match $pattern } | Select-Object -First 1
  if ($line -match "$name=([0-9.\-]+)") { return $Matches[1] }
  return '-'
}
Get-ChildItem (Join-Path $logs "$Filter.txt") | Sort-Object Name | ForEach-Object {
  $t = Get-Content $_.FullName -Raw
  $walls = [regex]::Matches($t, 'RESULT run=\d+ submissions=(\d+) wall_ms=([0-9.]+)') | ForEach-Object { [double]$_.Groups[2].Value }
  $subs  = [regex]::Match($t, 'RESULT run=0 submissions=(\d+)').Groups[1].Value
  $wall  = if ($walls) { '{0:N1}' -f (($walls | Measure-Object -Average).Average) } else { '-' }
  [pscustomobject]@{
    run          = $_.BaseName
    ref_p99      = Field $t 'window=idle period' 'period_ms_p99'
    frame_p99    = Field $t 'window=load period' 'period_ms_p99'
    frame_max    = Field $t 'window=load period' 'period_ms_max'
    missed       = Field $t 'window=load seconds' 'missed_vsyncs'
    prev_idle_50 = Field $t 'preview=idle preview_render_latency' 'preview_render_latency_ms_p50'
    prev_load_50 = Field $t 'preview=load preview_render_latency' 'preview_render_latency_ms_p50'
    prev_load_95 = Field $t 'preview=load preview_render_latency' 'preview_render_latency_ms_p95'
    prev_load_max = Field $t 'preview=load preview_render_latency' 'preview_render_latency_ms_max'
    export_ms    = $wall
    subs         = $subs
  }
} | Format-Table -AutoSize | Out-String -Width 250
