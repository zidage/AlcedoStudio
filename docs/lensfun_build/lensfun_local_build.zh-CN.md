# Lensfun 内置构建说明（Windows）

Alcedo Studio 现在会直接从上游子模块构建 Lensfun，源码位置在：

- `alcedo/src/third_party/lensfun`

现在不需要再单独配置或安装 Lensfun。顶层构建会在当前 CMake 构建目录下自动驱动一个独立的 Lensfun out-of-source 构建：

- `build/<preset>/third_party/lensfun`

## 必要准备

1. 初始化子模块：

```powershell
git submodule update --init --recursive alcedo/src/third_party/lensfun
```

2. 确保 Windows 下的 GLib2 依赖可用。

如果你使用仓库自带的 vcpkg toolchain 来配置 Alcedo Studio，bundled Lensfun 会优先尝试使用：

- `vcpkg/installed/<triplet>`

如果其中还没有安装 `glib`，先执行：

```powershell
.\vcpkg\vcpkg.exe install glib:x64-windows
```

不要安装 vcpkg 的 `lensfun` 端口。Alcedo 从 `alcedo_studio/src/third_party/lensfun` 构建 Lensfun。`vcpkg-overlays/lensfun` 会阻止该端口再安装一份互相冲突的 `lensfun.dll`。

如果你想改用非 vcpkg 的 GLib2 包，请在配置 Alcedo Studio 时显式传入 `ALCEDO_LENSFUN_GLIB2_BASE_DIR`。

## 顶层配置示例

```powershell
cmd /c scripts\msvc_env.cmd --preset win_debug `
  -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake" `
  -Deasy_profiler_DIR="$PWD/alcedo/third_party/easy_profiler-v2.1.0-msvc15-win64/lib/cmake/easy_profiler"
```

然后照常构建 Alcedo Studio：

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4
```

如果 Windows 下 bundled Lensfun 的配置阶段仍然失败，请先确认 Lensfun 子模块存在，并且 vcpkg 的 GLib2 头文件或你手动指定的目录存在：

```powershell
Test-Path .\alcedo\src\third_party\lensfun\CMakeLists.txt
Test-Path .\vcpkg\installed\x64-windows\include\glib-2.0\glib.h
# 或者，如果你传了覆盖路径：
Test-Path <你的-glib2-路径>
```
