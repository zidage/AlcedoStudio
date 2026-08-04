#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

preset="macos_release"
build_dir="${repo_root}/build/macos-release"
install_dir="${repo_root}/build/install"
package_out_dir="${repo_root}/build/macos-release/package"
bundle_name="AlcedoStudio"
jobs="8"
qt_prefix=""
require_metal_assets=1
codesign_identity="-"
codesign_options=""
codesign_options_set=0
codesign_timestamp="OFF"
codesign_timestamp_set=0

usage() {
  cat <<USAGE
Usage: $0 [options]

Build and package the macOS Alcedo Studio .app, DMG, and ZIP.

Options:
  --preset NAME              CMake configure/build preset (default: macos_release)
  --build-dir PATH           Build directory (default: build/macos-release)
  --install-dir PATH         CMake install prefix (default: build/install)
  --package-out-dir PATH     CPack output directory (default: build/macos-release/package)
  --bundle-name NAME         App bundle/executable name (default: AlcedoStudio)
  --qt-prefix PATH           Qt prefix containing bin/, lib/cmake/Qt6, plugins/, qml/
  --codesign-identity ID     macOS signing identity (default: '-' for ad-hoc; empty disables signing)
  --codesign-options VALUE   Semicolon-separated codesign options (default: empty for ad-hoc)
  --codesign-timestamp       Request a trusted timestamp when signing
  --no-codesign              Disable bundle signing
  --jobs N                   Parallel build jobs (default: 8)
  --skip-metal-asset-check   Do not require Metal metallib assets in verification
  -h, --help                 Show this help
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --preset)
      preset="$2"
      shift 2
      ;;
    --build-dir)
      build_dir="$2"
      shift 2
      ;;
    --install-dir)
      install_dir="$2"
      shift 2
      ;;
    --package-out-dir)
      package_out_dir="$2"
      shift 2
      ;;
    --bundle-name)
      bundle_name="$2"
      shift 2
      ;;
    --qt-prefix)
      qt_prefix="$2"
      shift 2
      ;;
    --codesign-identity)
      codesign_identity="$2"
      shift 2
      ;;
    --codesign-options)
      codesign_options="$2"
      codesign_options_set=1
      shift 2
      ;;
    --codesign-timestamp)
      codesign_timestamp="ON"
      codesign_timestamp_set=1
      shift
      ;;
    --no-codesign)
      codesign_identity=""
      shift
      ;;
    --jobs)
      jobs="$2"
      shift 2
      ;;
    --skip-metal-asset-check)
      require_metal_assets=0
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

if [[ -n "$codesign_identity" && "$codesign_identity" != "-" ]]; then
  if [[ "$codesign_options_set" -eq 0 ]]; then
    codesign_options="--options;runtime"
  fi
  if [[ "$codesign_timestamp_set" -eq 0 ]]; then
    codesign_timestamp="ON"
  fi
fi

duckdb_extensions_dir="${build_dir}/duckdb_extensions"

resolve_duckdb_extension() {
  local extension_name="$1"
  local env_name="$2"
  local extension_file_name="$3"
  local configured_path="${!env_name:-}"

  if [[ -n "$configured_path" ]]; then
    if [[ ! -f "$configured_path" ]]; then
      echo "${env_name} points to a missing file: ${configured_path}" >&2
      exit 1
    fi
    printf '%s' "$configured_path"
    return
  fi

  if ! command -v duckdb >/dev/null 2>&1; then
    echo "duckdb CLI is required to prepare ${extension_name}; install Homebrew duckdb or set ${env_name}." >&2
    exit 1
  fi

  mkdir -p "$duckdb_extensions_dir"
  duckdb -c "SET extension_directory='${duckdb_extensions_dir}'; INSTALL ${extension_name};" >/dev/null

  local resolved_path
  resolved_path="$(find "$duckdb_extensions_dir" -name "${extension_file_name}" -type f | head -n1)"
  if [[ -z "$resolved_path" ]]; then
    echo "Failed to locate installed ${extension_file_name} under ${duckdb_extensions_dir}" >&2
    find "$duckdb_extensions_dir" -type f >&2 || true
    exit 1
  fi

  printf '%s' "$resolved_path"
}

alcedo_duckdb_vss_extension="$(resolve_duckdb_extension vss ALCEDO_DUCKDB_VSS_EXTENSION vss.duckdb_extension)"
alcedo_duckdb_fts_extension="$(resolve_duckdb_extension fts ALCEDO_DUCKDB_FTS_EXTENSION fts.duckdb_extension)"
export ALCEDO_DUCKDB_VSS_EXTENSION="$alcedo_duckdb_vss_extension"
export ALCEDO_DUCKDB_FTS_EXTENSION="$alcedo_duckdb_fts_extension"

# Fail fast before a long configure/build if Neural Engine weights are missing.
demosaicnet_models_dir="${repo_root}/alcedo_studio/src/config/models"
for model in bayer.safetensors xtrans.safetensors; do
  model_path="${demosaicnet_models_dir}/${model}"
  if [[ ! -f "$model_path" ]]; then
    echo "Required DemosaicNet weight missing: ${model_path}" >&2
    echo "  These must be present so packaged apps can demosaic without a source tree." >&2
    exit 1
  fi
  model_size="$(wc -c <"$model_path" | tr -d ' ')"
  if [[ "$model_size" -lt 10240 ]]; then
    echo "DemosaicNet weight looks incomplete (${model_size} bytes): ${model_path}" >&2
    echo "  Restore the real safetensors blob before packaging." >&2
    exit 1
  fi
done

echo "========================================"
echo "  Alcedo Studio macOS Packager"
echo "========================================"
echo
echo "DuckDB VSS extension: ${ALCEDO_DUCKDB_VSS_EXTENSION}"
echo "DuckDB FTS extension: ${ALCEDO_DUCKDB_FTS_EXTENSION}"
echo

configure_args=(
  --preset "$preset"
  -B "$build_dir"
  "-DCMAKE_INSTALL_PREFIX=${install_dir}"
  "-DALCEDO_MACOS_BUNDLE=ON"
  "-DALCEDO_MACOS_BUNDLE_NAME=${bundle_name}"
  "-DALCEDO_MACOS_CODESIGN_IDENTITY=${codesign_identity}"
  "-DALCEDO_MACOS_CODESIGN_OPTIONS=${codesign_options}"
  "-DALCEDO_MACOS_CODESIGN_TIMESTAMP=${codesign_timestamp}"
  "-DALCEDO_DUCKDB_VSS_EXTENSION=${ALCEDO_DUCKDB_VSS_EXTENSION}"
  "-DALCEDO_DUCKDB_FTS_EXTENSION=${ALCEDO_DUCKDB_FTS_EXTENSION}"
)
if [[ -n "$qt_prefix" ]]; then
  configure_args+=("-DALCEDO_QT_PREFIX=${qt_prefix}")
fi

echo "Configuring CMake with preset '${preset}' ..."
printf '> cmake'
printf ' %q' "${configure_args[@]}"
printf '\n'
cmake "${configure_args[@]}"

echo
echo "Building install target ..."
echo "Note: Qt deployment uses macdeployqt and can take 10+ minutes on release builds."
build_args=(--build "$build_dir" --target install --parallel "$jobs")
printf '> cmake'
printf ' %q' "${build_args[@]}"
printf '\n'
cmake "${build_args[@]}"

echo
echo "Verifying install tree ..."
verify_args=(
  --install-dir "$install_dir"
  --bundle-name "$bundle_name"
)
if [[ "$require_metal_assets" -eq 0 ]]; then
  verify_args+=(--skip-metal-asset-check)
fi
"${script_dir}/verify_macos_install_tree.sh" "${verify_args[@]}"

echo
echo "Running CPack ..."
mkdir -p "$package_out_dir"
cpack_args=(--config "${build_dir}/CPackConfig.cmake" -B "$package_out_dir")
printf '> cpack'
printf ' %q' "${cpack_args[@]}"
printf '\n'
cpack "${cpack_args[@]}"

staging_root="${package_out_dir}/_CPack_Packages"
if [[ -d "$staging_root" ]]; then
  echo
  echo "Verifying CPack staging apps ..."
  while IFS= read -r -d '' staged_app; do
    staged_install_dir="$(dirname "$staged_app")"
    staged_verify_args=(
      --install-dir "$staged_install_dir"
      --bundle-name "$bundle_name"
    )
    if [[ "$require_metal_assets" -eq 0 ]]; then
      staged_verify_args+=(--skip-metal-asset-check)
    fi
    "${script_dir}/verify_macos_install_tree.sh" "${staged_verify_args[@]}"
  done < <(find "$staging_root" -name "${bundle_name}.app" -type d -print0)
fi

echo
echo "========================================"
echo "  Packaging Complete"
echo "========================================"
find "$package_out_dir" -maxdepth 1 \( -name '*.dmg' -o -name '*.zip' \) -print | sort
echo
