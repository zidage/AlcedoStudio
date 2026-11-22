#!/usr/bin/env bash
# VX1: every case on the NVIDIA and the Intel device. Logs go to build/tmp/vulkan_backend/vx1/.
# Run from the worktree root after the build of build/vx1.
set -u
root="$(pwd)"
runner="$root/build/vx1/vx1_runner.exe"
logs="$root/build/tmp/vulkan_backend/vx1"
mkdir -p "$logs"
status=0
for device in NVIDIA Intel; do
  for case_name in grade grade_photo film_print film_light_table film_blur; do
    log="$logs/${case_name}_${device}.txt"
    "$runner" --case "$case_name" --vk-device "$device" --cl-device "$device" \
      --spv "$root/build/vx1/spv" --src "$root/alcedo_studio/src" "$@" >"$log" 2>&1
    code=$?
    echo "exit $code" >>"$log"
    grep -E "^RESULT|^ERROR" "$log" || echo "$case_name $device: no RESULT line (exit $code)"
    [ "$code" -eq 0 ] || status=1
  done
done
exit $status
