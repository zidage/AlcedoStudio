# Bundled Lensfun Build Notes (Windows)

Alcedo Studio now builds Lensfun directly from the upstream submodule at:

- `alcedo/src/third_party/lensfun`

You do not need to configure or install Lensfun separately anymore. The top-level build drives a dedicated out-of-source Lensfun build under your active CMake build tree:

- `build/<preset>/third_party/lensfun`

## Required Setup

1. Initialize the submodule:

```powershell
git submodule update --init --recursive alcedo/src/third_party/lensfun
```

2. Make sure the Windows GLib2 dependency is available.

When you configure Alcedo Studio with the repository's vcpkg toolchain, the bundled Lensfun build will first try to use:

- `vcpkg/installed/<triplet>`

If `glib` is not installed there yet, install it first:

```powershell
.\vcpkg\vcpkg.exe install glib:x64-windows
```

Do not install the vcpkg `lensfun` port. Alcedo builds Lensfun from `alcedo_studio/src/third_party/lensfun`. The vcpkg overlay at `vcpkg-overlays/lensfun` keeps that port from installing a competing `lensfun.dll`.

If you want to use a non-vcpkg GLib2 package, pass `ALCEDO_LENSFUN_GLIB2_BASE_DIR` explicitly when configuring Alcedo Studio.

## Example Top-Level Configure

```powershell
cmd /c scripts\msvc_env.cmd --preset win_debug `
  -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake" `
  -Deasy_profiler_DIR="$PWD/alcedo/third_party/easy_profiler-v2.1.0-msvc15-win64/lib/cmake/easy_profiler"
```

Then build Alcedo Studio as usual:

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4
```

If the bundled Lensfun configure step still fails on Windows, verify the Lensfun submodule exists and that either the vcpkg GLib2 headers or your override path exists:

```powershell
Test-Path .\alcedo\src\third_party\lensfun\CMakeLists.txt
Test-Path .\vcpkg\installed\x64-windows\include\glib-2.0\glib.h
# or, if you passed an override:
Test-Path <your-glib2-path>
```
