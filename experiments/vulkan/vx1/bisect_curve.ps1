# VX1: the curve branch alone (behavior 7) in each source form, on one device: pipeline creation.
# Run from the worktree root after the build of build\vx1.
param([string]$Device = "Intel", [string]$Mask = "0x80u", [string]$Opt = "-O2")
$slangc = "$env:VULKAN_SDK\Bin\slangc.exe"
$spv = "build\vx1\spv"
foreach ($form in 0, 2) {
  foreach ($search in 0, 1) {
    foreach ($record in 0, 1) {
      $name = "primary_grade_f${form}_s${search}_r${record}${Opt}.spv"
      & $slangc experiments\vulkan\vx1\shaders\primary_grade.slang -I alcedo_studio\src\include `
        -target spirv -profile spirv_1_6 -fvk-use-scalar-layout -fvk-use-entrypoint-name $Opt `
        "-DVX1_BEHAVIOR_MASK=$Mask" "-DVX1_CURVE_FORM=$form" "-DVX1_CURVE_SEARCH=$search" `
        "-DVX1_RECORD_FORM=$record" -entry primary_grade_pointwise_rgba32f -o "$spv\$name" 2>&1 |
        Where-Object { $_ -notmatch "warning" }
      & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.3 --scalar-block-layout "$spv\$name" 2>&1 | Out-Null
      $valid = $LASTEXITCODE
      $out = & .\build\vx1\vx1_runner.exe --case pipeline --vk-device $Device --spv $spv --grade-spv $name 2>&1
      "form $form search $search record $record $Opt  spirv-val $valid  pipeline exit $LASTEXITCODE $($out | Select-String -Pattern 'PIPELINE OK|ERROR')"
    }
  }
}
