# Build from Source / 源码构建指南

This guide is synced with the current top-level `CMakeLists.txt` and `CMakePresets.json`.  
本指南已与当前顶层 `CMakeLists.txt` 和 `CMakePresets.json` 对齐。

## 1) Prerequisites / 前置要求

| English | 中文 |
| --- | --- |
| Linux x86_64 (CPU path), Windows 10/11 x64 (MSVC path), or macOS 13.3+ (Metal path). | Linux x86_64（CPU 路径）、Windows 10/11 x64（MSVC 路径）或 macOS 13.3+（Metal 路径）。 |
| CMake 3.21+ (CMake 4.x is supported by the Linux preset). | CMake 3.21+（Linux preset 已兼容 CMake 4.x）。 |
| Ninja and Git. | Ninja 和 Git。 |
| Qt 6.3+ with deployment tools (`qt_generate_deploy_qml_app_script`). | Qt 6.3+，并且包含部署工具（`qt_generate_deploy_qml_app_script`）。 |
| Windows: Visual Studio 2022 (MSVC x64), optional CUDA Toolkit 12.8+. | Windows：Visual Studio 2022（MSVC x64），可选 CUDA Toolkit 12.8+。 |
| macOS: Xcode Command Line Tools and Homebrew dependencies. | macOS：Xcode Command Line Tools 和 Homebrew 依赖。 |

Required Qt components from CMake:
- `Core`, `LinguistTools`, `Svg`, `Widgets`, `Quick`, `Qml`, `QuickControls2`, `QuickDialogs2`, `QuickEffects`
- `Test` (when `ALCEDO_BUILD_TESTS=ON`)
- `ShaderTools`, `GuiPrivate` (required by the editor RHI targets on desktop builds)
- `OpenGL` (used by the Qt Quick OpenCL/OpenGL backend)

## 2) Initialize Submodules / 初始化子模块

The current CMake layout requires the libultrahdr submodule. On Windows and
macOS, the bundled Lensfun and Metal-cpp sources are used when their source
trees are present; on Linux CPU builds, Lensfun can come from the system
development package and Metal-cpp is not needed.
当前 CMake 布局必须有 libultrahdr 子模块。Windows/macOS 在源码存在时使用内置
Lensfun 和 Metal-cpp；Linux CPU 构建可使用系统 Lensfun 开发包，不需要 Metal-cpp。

```bash
git submodule update --init --recursive \
  alcedo_studio/src/third_party/libultrahdr
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

## 4) Linux CPU and Intel OpenCL / Linux CPU 与 Intel OpenCL

Linux has two explicit bring-up presets. Both use Ninja and cap the build at
`-j4` through the preset definition. CUDA, Metal, WebGPU, and the optional model
downloader are not part of these Linux paths.
Linux 提供两个明确的 bring-up preset。两者都使用 Ninja，并通过 preset 将并行度固定为
`-j4`。CUDA、Metal、WebGPU 和可选模型下载器不属于这两条 Linux 路径。

Install the development packages supplied by your distribution. The common set
includes Qt 6 (`ShaderTools`, `GuiPrivate`, Quick Controls, and Quick Dialogs),
OpenCV, Eigen3, Exiv2, LibRaw, OpenImageIO, OpenColorIO, DuckDB, Lensfun, LittleCMS2,
xxHash, highway, OpenMP, pkg-config, Ninja, and CMake. If DuckDB or Lensfun is in a
non-standard prefix, pass it through `CMAKE_PREFIX_PATH`.
常用依赖包括 Qt 6（`ShaderTools`、`GuiPrivate`、Quick Controls、Quick Dialogs）、
OpenCV、Eigen3、Exiv2、LibRaw、OpenImageIO、OpenColorIO、DuckDB、Lensfun、LittleCMS2、
xxHash、highway、OpenMP、pkg-config、Ninja 和 CMake。若 DuckDB 或 Lensfun 安装在非标准
目录，需要通过 `CMAKE_PREFIX_PATH` 传入。

### 4.1 CPU fallback / CPU fallback

The CPU preset is the known-good baseline. It uses the CPU decoder/edit pipeline
and presents the final `CV_32FC4` frame by copying it into a QRhi `RGBA32F`
texture on the Qt Quick scene-graph thread. This keeps the UI usable when no Intel
GPU runtime is installed.
CPU preset 是已验证的基线。它使用 CPU 解码/编辑链路，并在 Qt Quick scene-graph 线程将
最终 `CV_32FC4` 帧复制到 QRhi `RGBA32F` 纹理；即使没有 Intel GPU runtime，UI 仍可工作。

```bash
cmake --preset linux_cpu_debug
cmake --build --preset linux_cpu_debug
```

For a short headless UI smoke test, use a platform available on the machine:

```bash
QT_QPA_PLATFORM=offscreen \
  build/linux-cpu-debug/alcedo_studio/src/ui/alcedo_main/alcedo_main --editor-backend=cpu
```

### 4.2 OpenCL compute and display / OpenCL 计算与显示

The OpenCL preset enables Linux OpenCL discovery and the existing OpenCL compute
targets. It supports GLX on X11 and EGL on Wayland/other EGL sessions. If GL
sharing is unavailable, the OpenCL pipeline downloads the final frame and uses
the same host-upload path as the CPU backend; this is a correctness fallback,
not a claim of zero-copy interop.
OpenCL preset 开启 Linux OpenCL 检测和现有 OpenCL 计算目标，并支持 X11 的 GLX 以及
Wayland/其他 EGL 会话。若 GL sharing 不可用，OpenCL 管线会读回最终帧并复用 CPU 的
host-upload 路径；这是正确性 fallback，不代表零拷贝互操作已经在所有驱动上成立。

When Linux OpenCL is compiled in but the normal default/settings backend cannot
initialize a usable runtime device, `alcedo_main` falls back to the CPU
host-upload backend. An explicit `--editor-backend=opencl` keeps the failure
visible for diagnostics. Linux OpenCL 编译启用但默认/设置的 OpenCL 运行时不可用时，
`alcedo_main` 会回退到 CPU host-upload；显式指定 `--editor-backend=opencl` 仍会保留
启动错误，便于诊断。

The build requires both the OpenCL development headers/loader and a real device
ICD. `libOpenCL.so` alone is only the loader. Verify the runtime before expecting
Intel acceleration:

```bash
clinfo -l
```

The command should list at least one Intel or Mesa OpenCL platform/device. Package
names vary by distribution: install the Khronos OpenCL headers/ICD loader and the
Intel Compute Runtime (or the distribution's Mesa Intel OpenCL runtime). If the
headers or loader are in a non-standard prefix, pass them at configure time, for
example:

```bash
cmake --preset linux_opencl_debug \
  -DOpenCL_INCLUDE_DIR=/path/to/opencl/include \
  -DOpenCL_LIBRARY=/path/to/libOpenCL.so
cmake --build --preset linux_opencl_debug
```

The preset is a compile/configure probe when no device is present; configure and
compile success does not prove that an ICD or GL-sharing context is available.
Run the OpenCL editor on a normal X11/Wayland desktop session. `QT_QPA_PLATFORM=offscreen`
does not provide the native GLX/EGL handles required for OpenCL/GL sharing.
没有设备时，preset 仍可用于配置和编译探测；配置/编译成功不等于 ICD 或 GL sharing
上下文可用。OpenCL 编辑器需要在正常 X11/Wayland 桌面会话运行；`QT_QPA_PLATFORM=offscreen`
不会提供 OpenCL/GL sharing 所需的原生 GLX/EGL handle。

The optional aria2c model downloader remains disabled. Enable
`-DALCEDO_BUILD_ARIA2C_FETCH=ON` or set `ALCEDO_ARIA2C_BINARY` when model
downloads are required. For an offline dependency cache, provide the ed25519
source tree through `-DFETCHCONTENT_SOURCE_DIR_ALCEDO_ED25519_SOURCE=/path/to/ed25519`;
otherwise CMake fetches it normally.

## 5) macOS (Metal + presets) / macOS（Metal + 预设）

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

## 6) Tests / 测试

| English | 中文 |
| --- | --- |
| Build with tests enabled (`win_debug`, `win_release_test`, or `macos_debug_tests`). | 使用启用测试的预设构建（`win_debug`、`win_release_test` 或 `macos_debug_tests`）。 |
| Run preset-based tests: `ctest --preset win_release_test` or `ctest --preset macos_debug_tests`. | 使用 preset 运行测试：`ctest --preset win_release_test` 或 `ctest --preset macos_debug_tests`。 |
| Traditional debug test command also works: `ctest --test-dir build/debug --output-on-failure`. | 传统命令同样可用：`ctest --test-dir build/debug --output-on-failure`。 |

## 7) Formatting and Tidy / 格式化与静态检查

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

## 8) Packaging / 打包

Windows:
```powershell
powershell -ExecutionPolicy Bypass -File scripts/package_windows.ps1 -BuildDir build/release -Preset win_release
```

The Windows and macOS package scripts allocate the update build number
automatically. They record a number only after successful packaging, so a
failed run retries the same number. Use `-BuildNumber N` or `--build-number N`
only to reproduce an exact build. VS Code provides separate stable and beta
package tasks for each platform.

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

Linux (native bring-up ZIP) / Linux（原生 bring-up ZIP）：
```bash
cmake --preset linux_cpu_debug
cmake --build --preset linux_cpu_debug_app
cmake --install build/linux-cpu-debug --prefix build/linux-cpu-install
cpack --config build/linux-cpu-debug/CPackConfig.cmake -B build/linux-cpu-package
```

The Linux package currently uses the repository's native CPack ZIP install rules
and is intended for bring-up verification. It does not bundle a system Qt/OpenCL
driver, and should be run on a machine with the required Linux runtime packages.
Arch packages, AppImage, Flatpak, and a Linux CI job are follow-up deliverables;
they are not implied by the native ZIP.

Linux 软件包目前使用仓库已有的原生 CPack ZIP 安装规则，仅用于移植 bring-up
验证；不会打包系统 Qt/OpenCL 驱动，因此运行时仍需安装 Linux 发行版依赖。
Arch 包、AppImage、Flatpak 和 Linux CI 仍是后续交付项，不能从原生 ZIP 推断已完成。

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

## 9) Frequently Used CMake Cache Options / 常用 CMake 缓存选项

| Option | English | 中文 |
| --- | --- | --- |
| `ALCEDO_ENABLE_CUDA` | Enable CUDA backend when toolkit is available. | 当工具链可用时启用 CUDA 后端。 |
| `ALCEDO_ENABLE_METAL` | Enable Metal backend on Apple platforms. | 在 Apple 平台启用 Metal 后端。 |
| `ALCEDO_ENABLE_OPENCL` | Enable OpenCL compute and Linux/Windows OpenCL editor support when the SDK/loader is found. | 找到 SDK/loader 时启用 OpenCL 计算和 Linux/Windows OpenCL 编辑器支持。 |
| `ALCEDO_ENABLE_WEBGPU` | Enable Dawn/WebGPU support on Windows. | 在 Windows 上启用 Dawn/WebGPU 支持。 |
| `ALCEDO_BUILD_TESTS` | Build tests/demos. | 构建测试与示例。 |
| `ALCEDO_DEPLOY_SOFTWARE_OPENGL` | Bundle `opengl32sw.dll` during Windows deploy. | Windows 部署时打包 `opengl32sw.dll`。 |
| `ALCEDO_QT_DEPLOY_TOOL_OPTIONS` | Semicolon-separated options passed to Qt deploy tool. | 传递给 Qt 部署工具的分号分隔参数。 |
| `ALCEDO_DAWN_SOURCE_DIR` | Dawn source path used by WebGPU build. | WebGPU 构建使用的 Dawn 源码路径。 |
| `PUERHLAB_LENSFUN_GLIB2_BASE_DIR` | Override GLib2 base dir for bundled Lensfun build on Windows. | 覆盖 Windows 上内置 Lensfun 构建使用的 GLib2 根目录。 |
