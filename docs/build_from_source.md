# Build from Source / 源码构建指南

This guide is synced with the current top-level `CMakeLists.txt` and `CMakePresets.json`.  
本指南已与当前顶层 `CMakeLists.txt` 和 `CMakePresets.json` 对齐。

## 1) Prerequisites / 前置要求

| English | 中文 |
| --- | --- |
| Windows 10/11 x64 (MSVC path) or macOS 13.3+ (Metal path). | Windows 10/11 x64（MSVC 路径）或 macOS 13.3+（Metal 路径）。 |
| CMake 3.21+ (preset minimum). | CMake 3.21+（preset 最低要求）。 |
| Ninja and Git. | Ninja 和 Git。 |
| Qt 6.3+ with deployment tools (`qt_generate_deploy_qml_app_script`). | Qt 6.3+，并且包含部署工具（`qt_generate_deploy_qml_app_script`）。 |
| Windows: Visual Studio 2022 (MSVC x64), optional CUDA Toolkit 12.8+. | Windows：Visual Studio 2022（MSVC x64），可选 CUDA Toolkit 12.8+。 |
| macOS: Xcode Command Line Tools and Homebrew dependencies. | macOS：Xcode Command Line Tools 和 Homebrew 依赖。 |

Required Qt components from CMake:
- `Core`, `LinguistTools`, `Svg`, `Widgets`, `Quick`, `Qml`, `QuickControls2`, `QuickDialogs2`, `QuickEffects`
- `Test` (when `ALCEDO_BUILD_TESTS=ON`)
- `ShaderTools`, `GuiPrivate` (when accelerated viewer backend is enabled)
- `OpenGL`, `OpenGLWidgets` (only when legacy OpenGL editor is enabled and CUDA is available)

## 2) Initialize Submodules / 初始化子模块

These submodules are required by the current CMake layout:
- `alcedo_studio/src/third_party/lensfun`
- `alcedo_studio/src/third_party/libultrahdr`
- `alcedo_studio/src/third_party/metal-cpp`

```powershell
git submodule update --init --recursive `
  alcedo_studio/src/third_party/lensfun `
  alcedo_studio/src/third_party/libultrahdr `
  alcedo_studio/src/third_party/metal-cpp
```

Note / 说明:
- `win_debug` and `win_release_test` default to `ALCEDO_ENABLE_WEBGPU=ON`, which requires a Dawn source checkout at `alcedo_studio/third_party/dawn` (or pass `-DALCEDO_ENABLE_WEBGPU=OFF`).
- `win_debug` 和 `win_release_test` 默认开启 `ALCEDO_ENABLE_WEBGPU=ON`，需要在 `alcedo_studio/third_party/dawn` 提供 Dawn 源码（或通过 `-DALCEDO_ENABLE_WEBGPU=OFF` 关闭）。

## 3) Windows (MSVC + presets) / Windows（MSVC + 预设）

Use the wrapper so MSVC/CUDA env vars are prepared first:
先使用封装脚本注入 MSVC/CUDA 环境变量：

```powershell
cmd /c scripts\msvc_env.cmd ...
```

### 3.1 Bootstrap vcpkg / 初始化 vcpkg

```powershell
.\vcpkg\bootstrap-vcpkg.bat
```

### 3.2 Debug build (`win_debug`) / 调试构建（`win_debug`）

```powershell
cmd /c scripts\msvc_env.cmd --preset win_debug `
  -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"

cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 8
```

### 3.3 Release build (`win_release`) / 发布构建（`win_release`）

`win_release` currently sets `ALCEDO_BUILD_TESTS=OFF` and `ALCEDO_ENABLE_WEBGPU=OFF`.
`win_release` 当前默认 `ALCEDO_BUILD_TESTS=OFF` 且 `ALCEDO_ENABLE_WEBGPU=OFF`。

```powershell
cmd /c scripts\msvc_env.cmd --preset win_release `
  -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"

cmd /c scripts\msvc_env.cmd --build --preset win_release --parallel 8
cmd /c scripts\msvc_env.cmd --install build/release --prefix build/install
```

DuckDB FTS is packaged from a local extension install, not from Git. If CMake
cannot find `fts.duckdb_extension`, install it locally with:

```powershell
duckdb -c "INSTALL fts;"
```

Then either reconfigure, or pass the matching extension path explicitly:

```powershell
cmd /c scripts\msvc_env.cmd --preset win_release `
  -DALCEDO_DUCKDB_FTS_EXTENSION="$env:USERPROFILE\.duckdb\extensions\v1.2.1\windows_amd64\fts.duckdb_extension"
```

The Windows packaging script can prepare the extension automatically when the
`duckdb` CLI is available.

### 3.4 Release tests preset (`win_release_test`) / 发布测试预设（`win_release_test`）

```powershell
cmd /c scripts\msvc_env.cmd --preset win_release_test `
  -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"

cmd /c scripts\msvc_env.cmd --build --preset win_release_test --parallel 8
ctest --preset win_release_test
```

## 4) macOS (Metal + presets) / macOS（Metal + 预设）

Install dependencies:
安装依赖：

```bash
brew install cmake ninja qt opencv opencolorio duckdb exiv2 glib libraw little-cms2 highway openimageio pkg-config xxhash eigen libomp
```

Debug app build (`macos_debug`):
调试构建（`macos_debug`）：

```bash
cmake --preset macos_debug
cmake --build --preset macos_debug --target alcedo_main
```

Release + package (`macos_release` + `macos_package`):
发布构建与打包（`macos_release` + `macos_package`）：

```bash
cmake --preset macos_release
cmake --build --preset macos_release
cmake --build --preset macos_package
```

If your Qt path differs from the preset default, override `ALCEDO_QT_PREFIX`:
若本地 Qt 路径与 preset 默认值不同，可覆盖 `ALCEDO_QT_PREFIX`：

```bash
cmake --preset macos_release -DALCEDO_QT_PREFIX=/path/to/Qt/6.x/macos
```

## 5) Tests / 测试

| English | 中文 |
| --- | --- |
| Build with tests enabled (`win_debug`, `win_release_test`, or `macos_debug_tests`). | 使用启用测试的预设构建（`win_debug`、`win_release_test` 或 `macos_debug_tests`）。 |
| Run preset-based tests: `ctest --preset win_release_test` or `ctest --preset macos_debug_tests`. | 使用 preset 运行测试：`ctest --preset win_release_test` 或 `ctest --preset macos_debug_tests`。 |
| Traditional debug test command also works: `ctest --test-dir build/debug --output-on-failure`. | 传统命令同样可用：`ctest --test-dir build/debug --output-on-failure`。 |

## 6) Formatting and Tidy / 格式化与静态检查

Windows:
```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target format
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target tidy
```

macOS/Linux:
```bash
cmake --build --preset macos_debug --target format
cmake --build --preset macos_debug --target tidy
```

## 7) Packaging / 打包

Windows:
```powershell
powershell -ExecutionPolicy Bypass -File scripts/package_windows.ps1 -BuildDir build/release -Preset win_release
```

Fallback manual packaging:
```powershell
cmd /c scripts\msvc_env.cmd --install build/release --prefix build/install
powershell -ExecutionPolicy Bypass -File scripts/verify_windows_install_tree.ps1 -InstallDir build/install
cpack --config build/release/CPackConfig.cmake
```

The install tree must include DemosaicNet weights under `bin/config/models/`, OpenCL
shader sources under `bin/opencl/`, and (when OpenCL shared libraries are enabled)
`OpenClContext.dll` / `OpenClProgramLibrary.dll`. The Khronos `OpenCL.dll` ICD loader
is optional when the GPU driver already provides one.

macOS:
```bash
# Preferred full pipeline (configure + install + verify + CPack DMG/ZIP):
bash scripts/package_macos.sh
# Or, after an existing macos_release configure:
cmake --build --preset macos_package
```

The macOS `.app` must be self-contained for a clean Apple Silicon Mac:

| Payload | Bundle path | Notes |
| --- | --- | --- |
| DemosaicNet weights | `Contents/MacOS/config/models/{bayer,xtrans}.safetensors` | Required for Neural Engine demosaic; checked by `scripts/verify_macos_install_tree.sh` |
| Metal libraries | `Contents/Resources/metallib/*.metallib` | Built with `xcrun metal` / `metallib` at build time (includes `demosaicnet_io.metallib`) |
| DuckDB extensions | `Contents/Resources/duckdb_extensions/` | `vss` + `fts` |
| Semantic sidecar | `Contents/MacOS/alcedo_mind` | Linked against system CoreML / Swift |
| aria2c | `Contents/MacOS/aria2c` | Optional model downloads (not DemosaicNet weights) |

Runtime resolution: Metal loads metallibs from the compile-time path first, then falls back to `Contents/Resources/metallib/` (see `ComputePipelineCache::ResolveMetallibPath`). DemosaicNet loads weights from `ALCEDO_DEMOASICNET_MODEL_DIR`, then `<exe>/config/models/`. Always re-run CMake configure after packaging-related CMake changes so install rules pick up new assets.

## 8) Frequently Used CMake Cache Options / 常用 CMake 缓存选项

| Option | English | 中文 |
| --- | --- | --- |
| `ALCEDO_ENABLE_CUDA` | Enable CUDA backend when toolkit is available. | 当工具链可用时启用 CUDA 后端。 |
| `ALCEDO_ENABLE_METAL` | Enable Metal backend on Apple platforms. | 在 Apple 平台启用 Metal 后端。 |
| `ALCEDO_ENABLE_WEBGPU` | Enable Dawn/WebGPU support on Windows. | 在 Windows 上启用 Dawn/WebGPU 支持。 |
| `ALCEDO_ENABLE_OPENGL_EDITOR` | Enable legacy OpenGL editor path. | 启用传统 OpenGL 编辑器路径。 |
| `ALCEDO_BUILD_TESTS` | Build tests/demos. | 构建测试与示例。 |
| `ALCEDO_DEPLOY_SOFTWARE_OPENGL` | Bundle `opengl32sw.dll` during Windows deploy. | Windows 部署时打包 `opengl32sw.dll`。 |
| `ALCEDO_QT_DEPLOY_TOOL_OPTIONS` | Semicolon-separated options passed to Qt deploy tool. | 传递给 Qt 部署工具的分号分隔参数。 |
| `ALCEDO_DAWN_SOURCE_DIR` | Dawn source path used by WebGPU build. | WebGPU 构建使用的 Dawn 源码路径。 |
| `PUERHLAB_LENSFUN_GLIB2_BASE_DIR` | Override GLib2 base dir for bundled Lensfun build on Windows. | 覆盖 Windows 上内置 Lensfun 构建使用的 GLib2 根目录。 |
