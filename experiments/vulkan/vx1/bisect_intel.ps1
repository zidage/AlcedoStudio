# VX1: which branch of the grade kernel makes vkCreateComputePipelines fail on the Intel driver.
# Run from the worktree root after the build of build\vx1.
param([string]$Device = "Intel", [string[]]$Masks = @())
$slangc = "$env:VULKAN_SDK\Bin\slangc.exe"
$spv = "build\vx1\spv"
if ($Masks.Count -eq 0) {
  # Every behavior alone, then all of them.
  $Masks = @(0, 1, 2, 3, 7, 8, 9, 11, 12 | ForEach-Object { "0x{0:X}" -f (1 -shl $_) }) + @("0xFFFFFFFF")
}
foreach ($mask in $Masks) {
  $name = "primary_grade_mask_$mask.spv"
  & $slangc experiments\vulkan\vx1\shaders\primary_grade.slang -I alcedo_studio\src\include `
    -target spirv -profile spirv_1_6 -fvk-use-scalar-layout -fvk-use-entrypoint-name -O2 `
    "-DVX1_BEHAVIOR_MASK=${mask}u" -entry primary_grade_pointwise_rgba32f -o "$spv\$name" 2>&1 |
    Where-Object { $_ -notmatch "warning" }
  $out = & .\build\vx1\vx1_runner.exe --case pipeline --vk-device $Device --spv $spv --grade-spv $name 2>&1
  "mask $mask exit $LASTEXITCODE $($out | Select-String -Pattern 'PIPELINE|ERROR|error')"
}
