#!/usr/bin/env bash
set -euo pipefail

# Build the downloader independently of the application. No Homebrew libraries enter the payload.
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work_dir="${repo_dir}/build/aria2-macos"
jobs=4
while [[ $# -gt 0 ]]; do
  case "$1" in
    --work-dir) work_dir="$2"; shift 2 ;;
    --jobs) jobs="$2"; shift 2 ;;
    *) echo "Usage: $0 [--work-dir PATH] [--jobs N]" >&2; exit 2 ;;
  esac
done
[[ "$(uname -s)" == Darwin && "$(uname -m)" == arm64 ]] || {
  echo "Build on a native arm64 Mac." >&2; exit 1;
}
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "Invalid job count: $jobs" >&2; exit 2; }
mkdir -p "$work_dir"
work_dir="$(cd "$work_dir" && pwd)"
available_kb="$(df -Pk "$work_dir" | awk 'END {print $4}')"
[[ "$available_kb" -ge 1048576 ]] || { echo "At least 1 GiB free is required." >&2; exit 1; }

cmake_bin="$(command -v cmake)"
sdk="$(xcrun --sdk macosx --show-sdk-path)"
export MACOSX_DEPLOYMENT_TARGET=13.3
export SDKROOT="$sdk"
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export CC="$(xcrun -f clang)" CXX="$(xcrun -f clang++)"
unset CPATH CPLUS_INCLUDE_PATH C_INCLUDE_PATH LIBRARY_PATH DYLD_LIBRARY_PATH
prefix="${work_dir}/prefix"
export PKG_CONFIG_PATH="${prefix}/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"
export CFLAGS="-O2 -arch arm64 -mmacosx-version-min=13.3 -isysroot ${sdk}"
export CXXFLAGS="$CFLAGS"
export CPPFLAGS="-I${prefix}/include"
export LDFLAGS="-arch arm64 -mmacosx-version-min=13.3 -isysroot ${sdk} -L${prefix}/lib"
mkdir -p "${work_dir}/downloads" "${work_dir}/dist" "${work_dir}/aria2-build"

fetch() {
  local name="$1" url="$2" sha="$3"
  local archive="${work_dir}/downloads/${name}"
  if [[ ! -f "$archive" ]]; then
    curl --fail --location --retry 3 --output "${archive}.part" "$url"
    mv "${archive}.part" "$archive"
  fi
  echo "${sha}  ${archive}" | shasum -a 256 --check
}
fetch aria2-1.37.0.tar.xz \
  https://github.com/aria2/aria2/releases/download/release-1.37.0/aria2-1.37.0.tar.xz \
  60a420ad7085eb616cb6e2bdf0a7206d68ff3d37fb5a956dc44242eb2f79b66b
fetch c-ares-1.34.5.tar.gz \
  https://github.com/c-ares/c-ares/releases/download/v1.34.5/c-ares-1.34.5.tar.gz \
  7d935790e9af081c25c495fd13c2cfcda4792983418e96358ef6e7320ee06346
for spec in aria2-1.37.0.tar.xz c-ares-1.34.5.tar.gz; do
  tar -xf "${work_dir}/downloads/${spec}" -C "$work_dir"
done

"$cmake_bin" -S "${work_dir}/c-ares-1.34.5" -B "${work_dir}/cares-build" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3 \
  -DCMAKE_OSX_SYSROOT="$sdk" -DCARES_STATIC=ON -DCARES_SHARED=OFF \
  -DCARES_BUILD_TESTS=OFF -DCARES_BUILD_TOOLS=OFF
"$cmake_bin" --build "${work_dir}/cares-build" --parallel "$jobs"
"$cmake_bin" --install "${work_dir}/cares-build"

configure_args=(
  --host=aarch64-apple-darwin --with-appletls --with-libcares --with-libz
  --without-openssl --without-gnutls --without-libnettle --without-libgmp
  --without-libgcrypt --without-libssh2 --without-sqlite3
  --without-libxml2 --without-libexpat --disable-bittorrent --disable-metalink
  --disable-nls --disable-shared --enable-static
)
cd "${work_dir}/aria2-build"
LIBCARES_CFLAGS="-I${prefix}/include" LIBCARES_LIBS="${prefix}/lib/libcares.a" \
  "${work_dir}/aria2-1.37.0/configure" "${configure_args[@]}"
make -j "$jobs"

payload="${work_dir}/payload"
mkdir -p "$payload"
install -m 755 src/aria2c "${payload}/aria2c"
strip -x "${payload}/aria2c"
codesign --force --sign - "${payload}/aria2c"
[[ "$(lipo -archs "${payload}/aria2c")" == arm64 ]]
version="$("${payload}/aria2c" --version)"
grep -q 'aria2 version 1.37.0' <<<"$version"
grep -q 'AppleTLS' <<<"$version"
while IFS= read -r dep; do
  case "$dep" in
    /usr/lib/*|/System/Library/*) ;;
    *) echo "Non-system dependency: $dep" >&2; exit 1 ;;
  esac
done < <(otool -L "${payload}/aria2c" | awk 'NR > 1 {print $1}')
cp "${work_dir}/aria2-1.37.0/COPYING" "$payload/"
cp "${work_dir}/c-ares-1.34.5/LICENSE.md" "${payload}/c-ares-LICENSE.md"
cp "${repo_dir}/scripts/build_aria2_macos.sh" "$payload/"
{
  echo "aria2 1.37.0; c-ares 1.34.5 (static); SDK zlib; deployment target 13.3"
  sw_vers
  uname -m
  xcodebuild -version
  "$CC" --version
  printf 'SDK: %s\nConfigure:' "$sdk"
  printf ' %s' "${configure_args[@]}"
  printf '\nCFLAGS: %s\nLDFLAGS: %s\n' "$CFLAGS" "$LDFLAGS"
  echo "$version"
  otool -L "${payload}/aria2c"
} > "${payload}/BUILD-INFO.txt"
archive="aria2-1.37.0-macos-arm64.tar.gz"
COPYFILE_DISABLE=1 tar -czf "${work_dir}/dist/${archive}" -C "$payload" \
  aria2c COPYING c-ares-LICENSE.md BUILD-INFO.txt build_aria2_macos.sh
# Publish the exact corresponding sources beside the binary (GPL-2.0-or-later).
COPYFILE_DISABLE=1 tar -czf "${work_dir}/dist/aria2-1.37.0-macos-arm64-sources.tar.gz" \
  -C "${work_dir}/downloads" aria2-1.37.0.tar.xz c-ares-1.34.5.tar.gz \
  -C "${repo_dir}/scripts" build_aria2_macos.sh
cd "${work_dir}/dist"
shasum -a 256 ./*.tar.gz > SHA256SUMS
cat SHA256SUMS
