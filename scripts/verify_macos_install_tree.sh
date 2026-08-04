#!/usr/bin/env bash
set -euo pipefail

install_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/build/install"
bundle_name="AlcedoStudio"
skip_metal_assets=0

usage() {
  cat <<USAGE
Usage: $0 [--install-dir PATH] [--bundle-name NAME] [--skip-metal-asset-check]

Verify that the macOS .app install tree contains the payload needed on a clean user Mac.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --install-dir)
      install_dir="$2"
      shift 2
      ;;
    --bundle-name)
      bundle_name="$2"
      shift 2
      ;;
    --skip-metal-asset-check)
      skip_metal_assets=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

app_dir="${install_dir}/${bundle_name}.app"
contents_dir="${app_dir}/Contents"
macos_dir="${contents_dir}/MacOS"
resources_dir="${contents_dir}/Resources"
frameworks_dir="${contents_dir}/Frameworks"
plugins_dir="${contents_dir}/PlugIns"

fail() {
  echo "[alcedo] macOS install tree verification failed: $*" >&2
  exit 1
}

assert_file() {
  [[ -f "$1" ]] || fail "missing required file: $1"
}

assert_dir() {
  [[ -d "$1" ]] || fail "missing required directory: $1"
}

assert_executable() {
  assert_file "$1"
  [[ -x "$1" ]] || fail "required executable is not executable: $1"
}

assert_dir "$app_dir"
assert_dir "$macos_dir"
assert_dir "$resources_dir"
assert_dir "$frameworks_dir"
assert_dir "$plugins_dir"

main_exe="${macos_dir}/${bundle_name}"
mind_exe="${macos_dir}/alcedo_mind"
aria2c_exe="${macos_dir}/aria2c"

assert_executable "$main_exe"
assert_executable "$mind_exe"
assert_executable "$aria2c_exe"

required_files=(
  "${contents_dir}/Info.plist"
  "${resources_dir}/qt.conf"
  "${macos_dir}/fonts/main_Inter.ttf"
  "${macos_dir}/fonts/main_NotoSans_zh.ttf"
  "${macos_dir}/config/icc/rec709_gamma22.icc"
  "${macos_dir}/config/models/bayer.safetensors"
  "${macos_dir}/config/models/xtrans.safetensors"
  "${resources_dir}/duckdb_extensions/vss.duckdb_extension"
  "${resources_dir}/duckdb_extensions/fts.duckdb_extension"
  "${frameworks_dir}/QtCore.framework/QtCore"
  "${frameworks_dir}/QtGui.framework/QtGui"
  "${frameworks_dir}/QtQml.framework/QtQml"
  "${frameworks_dir}/QtQuick.framework/QtQuick"
  "${frameworks_dir}/QtWidgets.framework/QtWidgets"
  "${plugins_dir}/platforms/libqcocoa.dylib"
)

for file in "${required_files[@]}"; do
  assert_file "$file"
done

if [[ "$skip_metal_assets" -eq 0 ]]; then
  metal_dir="${resources_dir}/metallib"
  assert_dir "$metal_dir"
  metal_libs=(
    metal_convert.metallib
    geometry_utils.metallib
    lens_calib.metallib
    fused_pipeline.metallib
    scope_analyzer.metallib
    to_linear_ref.metallib
    debayer_rcd.metallib
    highlight_reconstruct.metallib
    xtrans_interpolate.metallib
    cvt_ref_space.metallib
    demosaicnet_io.metallib
  )
  for lib in "${metal_libs[@]}"; do
    assert_file "${metal_dir}/${lib}"
  done
fi

if ! otool -L "$mind_exe" | grep -q '/System/Library/Frameworks/CoreML.framework/'; then
  fail "semantic sidecar is not linked against CoreML.framework: $mind_exe"
fi

if otool -L "$mind_exe" | grep -q '@rpath/libswift'; then
  if ! otool -l "$mind_exe" | grep -q 'path /usr/lib/swift (offset'; then
    fail "semantic sidecar has @rpath Swift dependencies but no /usr/lib/swift LC_RPATH"
  fi
fi

if command -v codesign >/dev/null 2>&1; then
  main_codesign_details="$(codesign -dv --verbose=4 "$main_exe" 2>&1 || true)"
  if grep -q 'Signature=adhoc' <<<"$main_codesign_details" &&
     grep -q 'flags=.*runtime' <<<"$main_codesign_details"; then
    fail "main executable is ad-hoc signed with hardened runtime; this can fail at launch when loading bundled Qt/framework dylibs. Use empty ALCEDO_MACOS_CODESIGN_OPTIONS for ad-hoc builds, or sign the full bundle with a Developer ID identity."
  fi
fi

declare -a macho_files=("$main_exe" "$mind_exe" "$aria2c_exe")
while IFS= read -r -d '' file; do
  macho_files+=("$file")
done < <(find "$macos_dir" "$frameworks_dir" "$plugins_dir" "${resources_dir}/duckdb_extensions" \
  \( -name '*.dylib' -o -name '*.so' -o -name '*.duckdb_extension' \) -print0 2>/dev/null || true)

while IFS= read -r -d '' framework; do
  framework_name="$(basename "$framework" .framework)"
  for candidate in \
    "${framework}/${framework_name}" \
    "${framework}/Versions/Current/${framework_name}" \
    "${framework}/Versions/"*/"${framework_name}"; do
    [[ -f "$candidate" ]] && macho_files+=("$candidate")
  done
done < <(find "$frameworks_dir" -maxdepth 1 -name '*.framework' -type d -print0 2>/dev/null || true)

bad_deps=()
missing_deps=()

has_swift_rpath() {
  otool -l "$1" 2>/dev/null | grep -q 'path /usr/lib/swift (offset'
}

verify_dep() {
  local owner="$1"
  local dep="$2"
  if [[ "$dep" = /* && -e "$dep" && "$dep" -ef "$owner" ]]; then
    return
  fi
  case "$dep" in
    /System/Library/*|/usr/lib/*)
      return
      ;;
    @rpath/*)
      local rel="${dep#@rpath/}"
      if [[ "$(basename "$owner")" == "$(basename "$rel")" ]]; then
        return
      fi
      if [[ -e "${frameworks_dir}/${rel}" || -e "${macos_dir}/${rel}" ]]; then
        return
      fi
      if [[ "$rel" == libswift*.dylib ]] && has_swift_rpath "$owner"; then
        return
      fi
      missing_deps+=("${owner} -> ${dep}")
      return
      ;;
    @executable_path/*)
      local rel="${dep#@executable_path/}"
      [[ -e "${macos_dir}/${rel}" ]] || missing_deps+=("${owner} -> ${dep}")
      return
      ;;
    @loader_path/*)
      local rel="${dep#@loader_path/}"
      local owner_dir
      owner_dir="$(dirname "$owner")"
      [[ -e "${owner_dir}/${rel}" ]] || missing_deps+=("${owner} -> ${dep}")
      return
      ;;
    /*)
      bad_deps+=("${owner} -> ${dep}")
      return
      ;;
  esac
}

for file in "${macho_files[@]}"; do
  [[ -f "$file" ]] || continue
  if ! file "$file" | grep -q 'Mach-O'; then
    continue
  fi
  while IFS= read -r dep; do
    [[ -n "$dep" ]] || continue
    verify_dep "$file" "$dep"
  done < <(otool -L "$file" 2>/dev/null | awk 'NR > 1 {print $1}')
done

if [[ "${#bad_deps[@]}" -gt 0 ]]; then
  printf '[alcedo] dependencies outside the bundle/system roots:\n' >&2
  printf '  %s\n' "${bad_deps[@]}" >&2
  exit 1
fi

if [[ "${#missing_deps[@]}" -gt 0 ]]; then
  printf '[alcedo] unresolved bundled dependencies:\n' >&2
  printf '  %s\n' "${missing_deps[@]}" >&2
  exit 1
fi

echo "[alcedo] macOS install tree verification passed: $app_dir"
