#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

clone_if_missing() {
  local path="$1"
  local marker="$2"
  local url="$3"
  shift 3
  if [[ -e "${path}/${marker}" ]]; then
    return
  fi
  mkdir -p "$(dirname "${path}")"
  rm -rf "${path}"
  git clone --depth 1 "$@" "${url}" "${path}"
}

copy_from_clone() {
  local url="$1"
  local source_rel="$2"
  local dest="$3"
  local tmp
  tmp="$(mktemp -d)"
  git clone --depth 1 "${url}" "${tmp}/repo"
  mkdir -p "$(dirname "${dest}")"
  cp -R "${tmp}/repo/${source_rel}" "${dest}"
  rm -rf "${tmp}"
}

download_file() {
  local url="$1"
  local dest="$2"
  mkdir -p "$(dirname "${dest}")"
  curl -fsSL "${url}" -o "${dest}"
}

clone_if_missing \
  "alcedo_studio/src/third_party/lensfun" \
  "CMakeLists.txt" \
  "https://github.com/lensfun/lensfun.git"

if [[ ! -e "alcedo_studio/src/third_party/libultrahdr/third_party/image_io/includes/image_io/base/data_segment_data_source.h" ]]; then
  git submodule sync -- "alcedo_studio/src/third_party/libultrahdr"
  git submodule update --init --recursive --depth 1 "alcedo_studio/src/third_party/libultrahdr"
fi

if [[ ! -e "alcedo_studio/src/third_party/libraw/libraw/libraw.h" ]]; then
  git submodule sync -- "alcedo_studio/src/third_party/libraw"
  git submodule update --init --recursive --depth 1 "alcedo_studio/src/third_party/libraw"
fi

# The CI preset forces PUERHLAB_USE_SYSTEM_GRPC_PROTOBUF=ON, so gRPC and protobuf
# are consumed from Homebrew (brew install grpc protobuf in the workflow) and the
# bundled source submodules are never built. Skip the recursive grpc/protobuf
# submodule init here — it only clones abseil/boringssl/cares/re2/zlib that would
# go unused and slow the prepare step down.

clone_if_missing \
  "alcedo_studio/src/third_party/metal-cpp" \
  "Metal/Metal.hpp" \
  "https://github.com/bkaradzic/metal-cpp.git"

if [[ ! -e "alcedo_studio/third_party/nlohmann_json/json.hpp" ]]; then
  copy_from_clone \
    "https://github.com/nlohmann/json.git" \
    "single_include/nlohmann/json.hpp" \
    "alcedo_studio/third_party/nlohmann_json/json.hpp"
fi

if [[ ! -e "alcedo_studio/third_party/stduuid/uuid.h" ]]; then
  copy_from_clone \
    "https://github.com/mariusbancila/stduuid.git" \
    "include/uuid.h" \
    "alcedo_studio/third_party/stduuid/uuid.h"
fi

if [[ ! -e "alcedo_studio/third_party/utfcpp/utf8.h" ]]; then
  copy_from_clone \
    "https://github.com/nemtrif/utfcpp.git" \
    "source/utf8.h" \
    "alcedo_studio/third_party/utfcpp/utf8.h"
  copy_from_clone \
    "https://github.com/nemtrif/utfcpp.git" \
    "source/utf8" \
    "alcedo_studio/third_party/utfcpp/utf8"
fi

if [[ ! -e "alcedo_studio/third_party/uuid_v4/uuid_v4.h" ]]; then
  download_file \
    "https://raw.githubusercontent.com/crashoz/uuid_v4/master/uuid_v4.h" \
    "alcedo_studio/third_party/uuid_v4/uuid_v4.h"
fi

if [[ ! -e "alcedo_studio/third_party/murmurhash3/MurmurHash3.cpp" ]]; then
  download_file \
    "https://raw.githubusercontent.com/aappleby/smhasher/master/src/MurmurHash3.cpp" \
    "alcedo_studio/third_party/murmurhash3/MurmurHash3.cpp"
  download_file \
    "https://raw.githubusercontent.com/aappleby/smhasher/master/src/MurmurHash3.h" \
    "alcedo_studio/third_party/murmurhash3/MurmurHash3.h"
fi
