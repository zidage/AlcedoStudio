#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
[[ "$(uname -s)" == Darwin && "$(uname -m)" == arm64 ]]
mkdir -p "${repo_dir}/build/tmp"
test_dir="$(mktemp -d "${repo_dir}/build/tmp/macos-architectures.XXXXXX")"
trap 'rm -rf "$test_dir"' EXIT
app="${test_dir}/AlcedoStudio.app"
mkdir -p "$app/Contents/"{MacOS,Resources/qml,Frameworks,PlugIns}
printf 'int main(void) { return 0; }\n' > "${test_dir}/main.c"
xcrun clang -arch x86_64 "${test_dir}/main.c" -o "${test_dir}/intel"
xcrun clang -arch arm64 "${test_dir}/main.c" -o "${test_dir}/native"
if cmake -DALCEDO_ARIA2C_BINARY="${test_dir}/intel" \
    -P "${repo_dir}/scripts/cmake/fetch_aria2.cmake" > "${test_dir}/override-result" 2>&1; then
  echo "Expected rejection of an Intel aria2c override." >&2; exit 1
fi
grep -F 'aria2c must contain arm64:' "${test_dir}/override-result"
for name in AlcedoStudio alcedo_mind alcedo-cli aria2c; do
  cp "${test_dir}/native" "$app/Contents/MacOS/$name"
done

expect_rejection() {
  local path="$1"
  if bash "${repo_dir}/scripts/verify_macos_install_tree.sh" --install-dir "$test_dir" \
      > "${test_dir}/result" 2>&1; then
    echo "Expected architecture rejection: $path" >&2; exit 1
  fi
  grep -F "Mach-O has no arm64 slice (x86_64): ${path}" "${test_dir}/result"
}

cp "${test_dir}/intel" "$app/Contents/MacOS/aria2c"
expect_rejection "$app/Contents/MacOS/aria2c"
cp "${test_dir}/native" "$app/Contents/MacOS/aria2c"
cp "${test_dir}/intel" "$app/Contents/Resources/qml/unexpected-plugin"
expect_rejection "$app/Contents/Resources/qml/unexpected-plugin"
echo "PASS: Intel aria2c and Mach-O files outside the former dependency list are rejected."
