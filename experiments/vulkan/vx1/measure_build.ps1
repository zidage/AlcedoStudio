# VX1 step 6: build time, SPIR-V size, source lines, and pipeline creation time of the Slang and
# GLSL kernels. Run from the worktree root after the build of build\vx1.
$bin = "$env:VULKAN_SDK\Bin"
$inc = "alcedo_studio\src\include"
$sh = "experiments\vulkan\vx1\shaders"
$spv = "build\vx1\spv"
$slang = @("-I", $inc, "-target", "spirv", "-profile", "spirv_1_6", "-fvk-use-scalar-layout",
           "-fvk-use-entrypoint-name", "-O2")
$jobs = @(
  @{ Name = "Slang primary_grade (1 entry)"; Exe = "$bin\slangc.exe";
     Args = @("$sh\primary_grade.slang") + $slang + @("-entry", "primary_grade_pointwise_rgba32f", "-o", "$spv\m_grade_slang.spv"); Out = "$spv\m_grade_slang.spv" },
  @{ Name = "Slang film_print (3 entries)"; Exe = "$bin\slangc.exe";
     Args = @("$sh\film_print.slang") + $slang + @("-entry", "film_print_exposure", "-entry", "film_print_first_amounts", "-entry", "film_print_display", "-o", "$spv\m_film_slang.spv"); Out = "$spv\m_film_slang.spv" },
  @{ Name = "GLSL primary_grade (glslc -O)"; Exe = "$bin\glslc.exe";
     Args = @("--target-env=vulkan1.3", "-O", "-I", $inc, "$sh\primary_grade.comp", "-o", "$spv\m_grade_glsl.spv"); Out = "$spv\m_grade_glsl.spv" }
)
foreach ($job in $jobs) {
  $times = @()
  for ($i = 0; $i -lt 5; ++$i) {
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    & $job.Exe @($job.Args) 2>&1 | Out-Null
    $watch.Stop()
    if ($LASTEXITCODE -ne 0) { "$($job.Name): compile failed"; break }
    $times += $watch.Elapsed.TotalMilliseconds
  }
  $sorted = $times | Sort-Object
  "{0,-32} median {1,6:N0} ms  min {2,6:N0} ms  SPIR-V {3,7:N0} bytes" -f $job.Name, $sorted[2], $sorted[0], (Get-Item $job.Out).Length
}
foreach ($file in "primary_grade.slang", "primary_grade.comp", "film_print.slang") {
  $lines = Get-Content "$sh\$file"
  $code = ($lines | Where-Object { $_.Trim() -ne "" -and $_.Trim() -notmatch '^//' }).Count
  "{0,-22} {1,4} lines, {2,4} code lines" -f $file, $lines.Count, $code
}
foreach ($device in "NVIDIA", "Intel") {
  foreach ($pair in @(@("m_grade_slang.spv", "primary_grade_pointwise_rgba32f"), @("m_grade_glsl.spv", "main"))) {
    & .\build\vx1\vx1_runner.exe --case pipeline --vk-device $device --spv $spv --grade-spv $pair[0] --grade-entry $pair[1] --no-validation 2>&1 |
      Select-String -Pattern "PIPELINE|ERROR"
  }
}
