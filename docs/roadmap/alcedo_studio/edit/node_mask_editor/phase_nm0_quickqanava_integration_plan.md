# Phase NM0 — QuickQanava Integration Baseline

Date: 2026-08-29

Status: complete — 固定 tag `2.50` 的 git submodule、CMake 静态 QML module 接入，以及 license 记录。

对应总体方案：[Node-aware Pipeline Editing and Mask Authoring 总体方案](../node_mask_editor_master_plan.md) 第 15.1 节和第 21.1 节。

NM0 没有执行子 Phase。本文件记录已经落地的依赖配置，不拆 `NM0.1` / `NM0.2`，也不描述 Nodes 面板或领域 mutation。

---

## 1. 固定 checkout

| 项 | 值 |
| --- | --- |
| Upstream | https://github.com/cneben/QuickQanava |
| Superproject path | `alcedo_studio/src/third_party/QuickQanava` |
| `.gitmodules` name | `alcedo_studio/src/third_party/QuickQanava` |
| URL | `https://github.com/cneben/QuickQanava.git` |
| Pin | tag `2.50` |
| Commit | `56bdf78d5b1d41fb60ae3b8ea2292df45787ecff` |
| Floating branch | 无。`.gitmodules` 不设 `branch`，configure 不 FetchContent |

初始化：

```powershell
git submodule update --init alcedo_studio/src/third_party/QuickQanava
```

macOS / Unix 相同命令：

```bash
git submodule update --init alcedo_studio/src/third_party/QuickQanava
```

该 checkout 没有嵌套 submodule。不要对它使用 `--recursive` 去拉别的仓库。

升级 pin 时：在 submodule 内 checkout 新的 tag 或 commit，更新本表、`THIRD_PARTY_NOTICE.txt` 和 `alcedo_studio/src/third_party/README.md`，不要改成跟踪 `master` / `develop`。

`.gitignore` 对 `alcedo_studio/src/third_party/*` 默认忽略；QuickQanava 已用 `!alcedo_studio/src/third_party/QuickQanava/` 放开。

---

## 2. CMake

入口：`alcedo_studio/src/third_party/CMakeLists.txt`。

- Cache path：`ALCEDO_QUICKQANAVA_SOURCE_DIR`，默认 `${CMAKE_CURRENT_SOURCE_DIR}/QuickQanava`。
- 缺少 `src/CMakeLists.txt` 或 `licence.txt` 时 `FATAL_ERROR`，提示 `git submodule update --init alcedo_studio/src/third_party/QuickQanava`。
- 不 `add_subdirectory` 上游 CMake。根文件会 `project(QuickQanava)`、把 `CMAKE_CXX_STANDARD` 设为 17、按选项编 Material 示例。`src/CMakeLists.txt` 的 `qt_wrap_cpp(qan_source_files, qan_header_files)` 在 Qt 6.9.3 上 configure 失败。
- Alcedo 用 `alcedo_studio/src/third_party/cmake/AlcedoQuickQanava.cmake` 对同一份 pin 源码调用 `qt_add_qml_module(QuickQanava STATIC URI QuickQanava ...)`。源文件列表必须与 pin 同步；缺文件则 `FATAL_ERROR`。
- QML URI：`QuickQanava`（`import QuickQanava` / `import QuickQanava as Qan`）。
- QML 文件用绝对路径加 `QT_RESOURCE_ALIAS`，binary 输出在 `${CMAKE_BINARY_DIR}/third_party/QuickQanava/QuickQanava`。不改全局 `QT_QML_OUTPUT_DIRECTORY`。
- 链接 `Qt6::Core`、`Qt6::Gui`、`Qt6::Qml`、`Qt6::Quick`、`Qt6::QuickControls2`、`Qt6::QuickEffects`。
- MSVC `/w`，其他编译器 `-w`。
- 不把 `QuickQanava` 链进 `alcedo_main`。生产 QML 在 NM5 之前不得 `import QuickQanava`。

生成的 CMake 目标：`QuickQanava`、`QuickQanavaplugin`。

静态 module 编进链接它的二进制；安装包不需要再拷一份 QML 源码树。真正随 `alcedo_main` 打包要到 NM5 链接之后。

---

## 3. License

QuickQanava 上游 `licence.txt` 标题写成 “BSD License 2.0”，正文是三条款 BSD（保留声明、二进制复述、禁止用作者/Destrat.io 名称背书）。Alcedo 按 **BSD-3-Clause** 记录。

Vendored [bezier](https://github.com/oysteinmyrmo/bezier) 在 `src/bezier/`，MIT。

已复制到：

- `third_party_licenses/QuickQanava-licence.txt`
- `third_party_licenses/QuickQanava-bezier-LICENSE.txt`

同时写入根 `NOTICE` 和 `THIRD_PARTY_NOTICE.txt` 条目 20b。

---

## 4. 上游 QML 与 Basic style

生产应用继续 `QQuickStyle::setStyle("Basic")`。不编上游 `samples/`。

库本身（不是 samples）里仍有：

| 文件 | 情况 |
| --- | --- |
| `src/CanvasNodeTemplate.qml` | `import QtQuick.Controls.Material` |
| `src/RectGroupTemplate.qml` | 使用 `Material.fontSize`，没有 Material import |
| 其余 module QML | `QtQuick.Controls`，无 Material import |

这是 pin `2.50` 的既有内容，NM0 不改上游文件。NM5 的 Alcedo delegate 必须自己画节点，不能用 `CanvasNodeTemplate.qml` 当生产外观。编这个静态 module 会让 qmlcachegen 看到 Material import；Qt 6.9.3 带 Material 控件，configure 需要它，但这不等于 Alcedo 改用 Material style。

---

## 5. 本变更不做的事

- 不创建 Nodes 轨页面、不写 `EditorNodeGraphProjection`、不接 `PipelineDocument`。
- 不把 QuickQanava 对象当成领域图。
- 不写只读 graph harness / QML TestCase。
- 不在 Windows 或 macOS 上做 install/package 加载证明。
- 不跟踪 `develop` / `master`。

这些属于 NM5 和 NM8。

---

## 6. 主要调用链

NM0 没有用户操作。构建时的路径：

```text
CMake configure
  -> alcedo_studio/src/third_party/CMakeLists.txt
  -> require ALCEDO_QUICKQANAVA_SOURCE_DIR checkout
  -> include AlcedoQuickQanava.cmake
  -> qt_add_qml_module(QuickQanava STATIC URI QuickQanava)
  -> targets QuickQanava / QuickQanavaplugin
```

Checkout 缺失：

```text
missing src/CMakeLists.txt or licence.txt
  -> FATAL_ERROR
  -> tell operator to git submodule update --init alcedo_studio/src/third_party/QuickQanava
  -> configure does not FetchContent and does not skip the module
```

---

##### Phase NM0 completion record (2026-08-29)

**Status:** complete — pinned QuickQanava `2.50` submodule, CMake static QML module, license notices. No production import.

**Primary success call chain:**

```text
git submodule update --init alcedo_studio/src/third_party/QuickQanava
  -> CMake finds src/CMakeLists.txt and licence.txt
  -> include AlcedoQuickQanava.cmake
  -> QuickQanava static QML module targets exist
```

**Primary failure call chain:**

```text
submodule not initialized
  -> FATAL_ERROR from alcedo_studio/src/third_party/CMakeLists.txt
  -> configure stops; no online fetch; no sample build
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| Pin tag `2.50` at `56bdf78d5b1d41fb60ae3b8ea2292df45787ecff` | git submodule gitlink | PASS |
| CMake creates and compiles `QuickQanava` | `QuickQanava.lib` | PASS |
| Production `alcedo_main` does not import QuickQanava | source inspection | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target QuickQanava
```

Configure: `-- Configuring done (35.1s)` then `-- Generating done (6.4s)`.
Build: 127/127, linked `alcedo_studio/src/third_party/QuickQanava.lib`. MSVC D9025 (`/W4` overridden by `/w`) is expected.

Suite totals: 3/3 for the NM0 pin checks above. No QML TestCase in this change.

**Checklist / exit condition:** dependency pin and CMake wiring done. Original master-plan harness and Windows/macOS package load are not claimed here.

**LOC note (grill-code-review):** first-party edits are CMake, gitignore, notices, and this record. Upstream tree is the submodule checkout.

**Residual gaps:** no QML import harness; `alcedo_main` does not link `QuickQanava`; macOS compile not run on this machine; `CanvasNodeTemplate.qml` still imports Material in upstream.

##### Phase NM0 completion record (2026-08-29, macOS)

**Status:** complete — same pin and Alcedo CMake wrapper; macOS Clang debug configured and linked. No production import. No CMake source change required for Clang.

**Primary success call chain:**

```text
git submodule update --init alcedo_studio/src/third_party/QuickQanava
  -> cmake --preset macos_debug
  -> include AlcedoQuickQanava.cmake
  -> qt_add_qml_module(QuickQanava STATIC URI QuickQanava)
  -> cmake --build --preset macos_debug --target QuickQanava
  -> libQuickQanava.a + qmldir under build/macos-debug/third_party/QuickQanava/QuickQanava
```

**Primary failure call chain:**

```text
submodule not initialized
  -> FATAL_ERROR from alcedo_studio/src/third_party/CMakeLists.txt
  -> configure stops; no online fetch; no sample build
```

macOS CI now inits the same gitlink from `scripts/ci_prepare_third_party.sh` (no `--recursive`).

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| Pin tag `2.50` at `56bdf78d5b1d41fb60ae3b8ea2292df45787ecff` | git submodule gitlink | PASS |
| CMake creates and compiles `QuickQanava` | `libQuickQanava.a` | PASS |
| Static plugin and QML module metadata | `libQuickQanavaplugin.a`, `qmldir` | PASS |
| Production `alcedo_main` does not import QuickQanava | source inspection | PASS |

Commands:

```text
git submodule update --init alcedo_studio/src/third_party/QuickQanava
cmake --preset macos_debug
cmake --build --preset macos_debug --target QuickQanava --parallel 8
cmake --build --preset macos_debug --target QuickQanavaplugin --parallel 8
```

Configure: `-- Configuring done (2.7s)` then `-- Generating done (0.6s)`.
Build: 127/127, linked `alcedo_studio/src/third_party/libQuickQanava.a`. Plugin linked `third_party/QuickQanava/QuickQanava/libQuickQanavaplugin.a`. qmltyperegistration warns that `gtpo::graph` / `gtpo::node` bases are not found; expected for this pin.

Host: macOS 26.5.2, Homebrew clang 21.1.1, Homebrew Qt 6.9.2 (Windows NM0 used Qt 6.9.3).

**Checklist / exit condition:** macOS configure/build of the pinned module done. Harness and package load still not claimed.

**Residual gaps:** no QML import harness; `alcedo_main` does not link `QuickQanava`; `CanvasNodeTemplate.qml` still imports Material in upstream.
