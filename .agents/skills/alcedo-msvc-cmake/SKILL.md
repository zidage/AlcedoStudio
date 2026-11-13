---
name: alcedo-msvc-cmake
description: Use when working on alcedo with CMake on Windows/MSVC, especially when the user mentions MSVC, Windows, presets, Ninja, CUDA, or scripts/msvc_env.cmd, or when an agent would otherwise run bare cmake commands in this repository.
---

# Alcedo Studio MSVC CMake

Use this skill for configure, build, install, and test workflows in this repository on Windows.

## Workflow

- Run commands from the repository root.
- Prefer the presets in `CMakePresets.json`: `win_debug` and `win_release`.
- Do not invoke bare `cmake` directly for configure/build/install when `scripts/msvc_env.cmd` is available.
- Use `cmd /c scripts\msvc_env.cmd ...` so Visual Studio, MSVC, and CUDA environment variables are initialized first.
- Treat MSVC configure/build/test commands in this repository as long-running by default. Typical compile times can reach about 20 minutes, so use generous command timeouts instead of short defaults.

## Command Templates

- Configure debug: `cmd /c scripts\msvc_env.cmd --preset win_debug`
- Configure release: `cmd /c scripts\msvc_env.cmd --preset win_release`
- Build debug: `cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4`
- Build release: `cmd /c scripts\msvc_env.cmd --build --preset win_release --parallel 4`
- Install release: `cmd /c scripts\msvc_env.cmd --install build/release --prefix build/install`
- Run tests after a debug build: `ctest --test-dir build/debug --output-on-failure`

## Rules

- Append user-provided `-D...` cache entries to the configure command after the preset.
- If the user asks for a `cmake --build build/...` style command, translate it to the wrapper form instead of changing the build intent.
- If the wrapper fails before reaching CMake, inspect `scripts/msvc_env.cmd` before changing presets or toolchain arguments.
- Prefer timeout budgets on the order of tens of minutes for MSVC build/test invocations in this repo; avoid retry loops caused by timeouts that are too short.
