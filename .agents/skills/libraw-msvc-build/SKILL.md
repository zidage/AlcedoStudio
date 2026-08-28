---
name: libraw-msvc-build
description: Use when building the LibRaw submodule (alcedo_studio/src/third_party/LibRaw) on Windows/MSVC — i.e. when the user mentions LibRaw, libraw.dll, libraw_static.lib, raw-identify, dcraw_emu, Makefile.msvc, or asks to compile/build the third-party LibRaw tree. LibRaw has NO in-repo CMakeLists.txt; it builds via nmake + Makefile.msvc.
---

# LibRaw MSVC Build

Build the LibRaw third-party library (`alcedo_studio/src/third_party/LibRaw`) on Windows with the
MSVC toolchain. This is the build path that actually works in this repo — there is **no**
`CMakeLists.txt` inside the LibRaw tree (per LibRaw's `README.cmake`, the CMake scripts live in a
separate `LibRaw/LibRaw-cmake` repo), so do **not** attempt `cmake` here. Use
`nmake -f Makefile.msvc`.

## Toolchain

- Visual Studio 2022 (Community), x64. Located via `vswhere`.
- The dev environment (cl.exe / nmake.exe / INCLUDE / LIB) must be injected before building. The outer repo provides `scripts/msvc_env.cmd` for the CMake-based Alcedo build, but it forwards to `cmake` and is useless for LibRaw's nmake path.

## The env-injecting wrapper

A wrapper lives in the LibRaw directory and handles env injection + nmake for you:

```
alcedo_studio/src/third_party/LibRaw/msvc_build.cmd
```

It mirrors `scripts/msvc_env.cmd`'s injection logic (vswhere → `VsDevCmd.bat -arch=x64 -host_arch=x64`) and then runs `nmake -f Makefile.msvc`. **If `msvc_build.cmd` is missing** (e.g. the submodule was refreshed), recreate it from the recipe in the "Recreating the wrapper" section below — do not fall back to bare `cmake`.

## Build commands

Run from the LibRaw directory (`alcedo_studio/src/third_party/LibRaw`). Use the `cmd` form so the
`.cmd` wrapper executes correctly:

- Build everything (default `all` = DLL + static lib + all sample exes):
  ```
  cmd /c "alcedo_studio\src\third_party\LibRaw\msvc_build.cmd"
  ```
- Build only the static library:
  ```
  cmd /c "alcedo_studio\src\third_party\LibRaw\msvc_build.cmd" lib\libraw_static.lib
  ```
- Build only the DLL (+ import lib):
  ```
  cmd /c "alcedo_studio\src\third_party\LibRaw\msvc_build.cmd" bin\libraw.dll
  ```
- Clean:
  ```
  cmd /c "alcedo_studio\src\third_party\LibRaw\msvc_build.cmd" clean
  ```

The wrapper forwards any extra arguments as nmake targets, so you can pass arbitrary `Makefile.msvc` targets (e.g. `bin\raw-identify.exe`).

## Artifacts

After a full build (paths relative to `alcedo_studio/src/third_party/LibRaw`):

| Artifact | Path |
|---|---|
| Dynamic library | `bin\libraw.dll` |
| DLL import library | `lib\libraw.lib` |
| DLL export table | `lib\libraw.exp` |
| Static library | `lib\libraw_static.lib` |
| Sample executables | `bin\*.exe` (raw-identify, dcraw_emu, simple_dcraw, 4channels, unprocessed_raw, mem_image, multirender_test, postprocessing_benchmark, openbayer_sample, rawtextdump, dcraw_half, half_mt) |

The sample exes link against `libraw.dll`. A quick executable check is
`bin\raw-identify.exe` with no arguments. It should print a usage banner; if it runs, the DLL
loads and links correctly.

## Build options (currently all OFF)

`Makefile.msvc` has optional integrations, all commented out at the top of the file. The default build has **no** LCMS, JPEG, DNG SDK, or RawSpeed:

- LCMS 1.x: `LCMS_DEF` / `LCMS_LIB`
- LCMS 2.x: `LCMS_DEF` / `LCMS_LIB`
- JPEG (for DNG): `JPEG_DEF` / `JPEG_LIB`
- DNG SDK: `CFLAGS_DNG` / `LDFLAGS_DNG`
- RawSpeed: `CFLAGS_RAWSPEED` / `LDFLAGS_RAWSPEED`
- SIMD: `COPT_OPT` (e.g. `/arch:AVX`)
- Compiler flags line: `CC=cl.exe` / `COPT=/EHsc /MP /MD /I. /DWIN32 /O2 /W0 /nologo ...`

To enable an option, uncomment the relevant `*_DEF`/`*_LIB` lines at the top of `Makefile.msvc` and point them at the real dependency paths (Alcedo resolves most of these via vcpkg). Edit the Makefile in place — do not try to pass them as nmake args (the variables are not exposed on the command line).

## Notes & rules

- Build is long-ish: ~80 translation units compiled with `/MP`. Use generous timeouts (≥ 10 minutes); avoid retry loops from short timeouts.
- `/W0` suppresses warnings by design — a clean build is expected to emit only the source-file echo lines.
- The DLL build deletes `bin\libraw.dll` / `lib\libraw.lib` first, and `clean` removes objects + libs + exes. These `del` "Could Not Find" messages on a fresh tree are normal, not errors.
- `dcraw_half.c` and `half_mt_win32.c` are C (not C++); everything else is `.cpp`. `unprocessed_raw.exe` additionally links `ws2_32.lib`.
- This is distinct from the main Alcedo build (see the `alcedo-msvc-cmake` skill). LibRaw here is built standalone via nmake; do not route it through `scripts/msvc_env.cmd` (that forwards to `cmake`).

## Recreating the wrapper

If `alcedo_studio/src/third_party/LibRaw/msvc_build.cmd` is absent, recreate it. It must, from the
LibRaw directory:

1. `cd /d "%~dp0"` so it always runs from its own location (the LibRaw root).
2. Locate VS via `%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe` with `-latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath` (fall back to dropping the `-requires` clause if that returns nothing).
3. Call `<VSINSTALL>\Common7\Tools\VsDevCmd.bat -arch=x64 -host_arch=x64` (fall back to `<VSINSTALL>\VC\Auxiliary\Build\vcvarsall.bat x64`).
4. Verify `nmake.exe` and `cl.exe` are on PATH; fail loudly if not.
5. Run `nmake -f Makefile.msvc all` (or forwarded args as nmake targets) and propagate the exit code.

Reference implementation is committed at
`alcedo_studio/src/third_party/LibRaw/msvc_build.cmd`.
