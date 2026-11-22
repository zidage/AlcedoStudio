#!/bin/sh
# VX1 step 5, on the Mac: compile the MSL that slangc -target metal wrote. Run in the folder of the
# .metal files.
for f in probe primary_grade; do
  xcrun -sdk macosx metal -std=metal3.0 -c "$f.metal" -o "$f.air" >"$f.log" 2>&1
  code=$?
  echo "$f: exit $code, $(grep -c 'error:' "$f.log") errors, $(grep -c 'warning:' "$f.log") warnings"
  head -12 "$f.log"
done
