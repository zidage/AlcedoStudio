# 色轮重做：归一化影调 Lift/Gamma/Gain + Offset

Date: 2026-10-18

Status: P1–P5 已实施（2026-10-18），PR 待合并。Issue：[#445](https://github.com/zidage/AlcedoStudio/issues/445)。
用户决定：一次完成 P1–P5；不保留旧公式的渲染分支，旧工程的色轮参数在读入时转换为新解释。

## 1. 问题

目前色轮把 `out = (c + lift)^gamma × gain` 直接作用在 ACEScc 编码值 `c` 上
（`cuda_primary_grade_pass.cu`、`primary_grade.cl`、`primary_grade.metal` 三份实现相同）。
数学上符合 CDL，但 ACEScc 不是 0=黑、1=白 的归一化信号，三个操作的支点都落在可见范围之外：

| 色轮 | 设计意图 | 在 ACEScc 中的实际行为 |
|---|---|---|
| Lift | 白点不动，主要推暗部 | log 域加法 = 线性域乘法，对所有影调完全一样（与 Exposure 滑块同一数学） |
| Gain | 黑点不动，主要推亮部 | 支点在 ACEScc 0 = 线性 0.0012（中灰 −7.2EV） |
| Gamma | 两端不动，推中间调 | 支点在线性 0.0012 和 222 |

数值模拟（色盘拉满偏红，strength 0.10，R/G 比变化，单位：档）：

| 影调 | Lift | Gamma | Gain |
|---|---|---|---|
| −3EV | 2.15 | 0.76 | 0.52 |
| 中灰 | 2.15 | 0.80 | 0.89 |
| +2EV | 2.15 | 0.73 | 1.13 |

结论：在 −3EV 到 +2EV 之间三个轮几乎都是整体平移；Lift 的力度约是另外两个的 2.7 倍；
`strength` 被 clamp 在 `[0, 0.10]`，等于固定值。界面直接显示 "Lift/Gamma/Gain"，没有说明。

## 2. 目标

1. 在归一化影调坐标里实现真正的 Lift/Gamma/Gain，各自集中作用于暗部、中间调、高光。
2. 新增 Offset（整体）轮，数学上等于旧 Lift：每个通道的影调整体平移。
3. 力度统一用"档（EV）"标定：每个轮子在自己的目标影调上，满幅效果相同。
4. 界面改名为 暗部 / 中间调 / 高光 / 整体，保留英文术语作副标题，并加 tooltip。
5. 旧工程靠版本字段保持原有渲染结果；没动过色轮的旧工程无感升级。

不在范围内：ASC CDL 导入导出、色轮的 HDR 专用模式、可调影调范围的 UI（常量先固定，
但由 CPU 下发给 kernel，以后可以开放）。

## 3. 数学定义（v2）

### 3.1 影调坐标

工作空间仍为 ACEScc。以中灰为 0EV：

```
ev = (c − CE_ACESCC_MIDDLE_GREY) × CE_ACESCC_B
t  = (ev − kToneBlackEv) / (kToneWhiteEv − kToneBlackEv)
```

常量：`kToneBlackEv = −6`，`kToneWhiteEv = +6`（范围 R = 12 档，中灰正好在 t = 0.5）。
按通道运算完再按同一公式映射回 ACEScc。

### 3.2 各轮（逐通道，参数 K 单位为档）

| 轮 | 公式 | 不动点 | 标定点（满幅 = K 档） |
|---|---|---|---|
| 暗部 Lift | `t' = t + (K/R) · clamp(1 − t, 0, 1)` | t ≥ 1 | t ≤ 0（−6EV 及以下） |
| 中间调 Gamma | `t' = t^p`，`p = ln(0.5 + K/R) / ln(0.5)`；t ∉ (0,1) 不变 | t ≤ 0、t ≥ 1 | t = 0.5（中灰） |
| 高光 Gain | `t' = t + (K/R) · max(t, 0)` | t ≤ 0 | t = 1（+6EV），以上按比例继续 |
| 整体 Offset | `ev' = ev + K` | 无 | 所有影调 |

`p` 和 `K/R` 都在 CPU（`adjustment_runtime.cpp`）里按通道算好再下发，kernel 只做查表级运算。
`0.5 + K/R` 需要 clamp 到 `> 0`（K 的取值范围远小于 6，正常不会触发）。

模拟（每个轮 +1 档，单位 ΔEV）：

| EV | 暗部 | 中间调 | 高光 | 整体 |
|---|---|---|---|---|
| −6 | 1.00 | 0.00 | 0.00 | 1.00 |
| −4 | 0.83 | 0.98 | 0.17 | 1.00 |
| −2 | 0.67 | 1.11 | 0.33 | 1.00 |
| 0 | 0.50 | 1.00 | 0.50 | 1.00 |
| +2 | 0.33 | 0.75 | 0.67 | 1.00 |
| +4 | 0.17 | 0.41 | 0.83 | 1.00 |
| +6 | 0.00 | 0.00 | 1.00 | 1.00 |

中间调峰值略偏暗部（−2EV 处 1.11），这是幂函数本身的形状，可以接受。

### 3.3 色盘与 Master 到 K 的映射

- 色盘：沿用 `DiscToCdlDelta` 的色相方向单位向量 `d`（三通道和为 0），
  `K_color[ch] = kDiscMaxStops × r × d[ch]`。`kDiscMaxStops = 1.0`：拉满时主导通道约 +0.82 档、
  另两个约 −0.41 档，R/G 比约 1.2 档。
- Master：`K_master ∈ [−kMasterMaxStops, +kMasterMaxStops]`，`kMasterMaxStops = 2.0`，
  三通道相同，与色盘相加：`K[ch] = K_color[ch] + K_master`。
- 四个轮子使用同一组常量，Gamma 不再需要单独的符号翻转（向红拖 = 中间调变红；Master 正 = 变亮）。

常量放在一个 Qt-free 头文件里，见 4.1。数值先按上表定，P4 里实际看图后可以微调，
改常量不影响存储格式。

## 4. 数据与兼容

### 4.1 Model（`color_wheel_model.hpp/.cpp`）

- 只有一种渲染公式（第 3 节），GPU kernel 中没有旧公式分支。
- `ColorWheelPayload` 新增 `ColorWheelControl offset`。四个轮的 `color_offset` 和
  `luminance_offset` 都以档为单位，中性值为 0：`color_offset` 是色盘换算出的逐通道档数，
  `luminance_offset` 是 Master 档数，`disc` 是色盘位置，`strength` 是面板写入的色盘满幅档数。
- 每个通道的 K 值 = `color_offset[ch] + luminance_offset`。
- `ToJson` 写出 `"version": 2`（持久化格式版本）。`LoadJson` 读到没有 `version` 的 JSON 时，
  按旧格式解释并立即转换（4.2），转换后的 payload 只有新解释。

### 4.2 旧格式参数的读入转换

旧公式：`c' = ((c + lift)^gamma) × gain`，作用在 ACEScc 上。新公式的执行顺序为
offset → lift → gamma → gain，所以：

- 旧 lift → 新 offset：精确转换，`K = (color_offset[ch] + luminance_offset) × CE_ACESCC_B`。
- 旧 gamma/gain → 新 lift/gamma/gain：对每个通道，在 −6…+6EV（步长 0.5EV）的输入采样点上，
  以旧公式（含旧 lift）的输出为目标，对新公式的 3 个 K 值做最小二乘拟合（Gauss-Newton，
  数值雅可比）。旧 gain 单独使用时 ΔEV 是 EV 的线性函数，可由新 lift + gain 精确表示。
  模拟误差：常见取值不超过 0.03 档；旧 lift 很大（约 1.4 档）且同时调了 gamma 时，范围两端
  可达约 0.12 档（新 Gamma 在 ±6EV 处不起作用）。
- 拟合出的逐通道 K 写入 `color_offset`（去均值部分）和 `luminance_offset`（三通道均值），
  不做截断，渲染结果就是拟合结果。`disc` 按去均值部分的方向和大小反推，超出单位圆时截断
  到圆上；用户下次拖动该色盘时，`color_offset` 按新的色盘位置重新计算。

### 4.3 旧工程的字节一致性（root id 与 history）

- 工程的 root id 是对解码后的文档重新序列化再求 hash（`ComputeRootId`），`TryDecodeRootState`
  会与存储的 root id 比较；history 回放时，`SetParameter` 的 before 值要与当前 Model JSON 逐字节
  相等。所以未编辑的旧工程必须序列化成与原来完全相同的字节。
- 做法：`ColorWheelPayload::version_one_json` 保存读入的 version 1 JSON。渲染使用转换后的值；
  `ToJson` 在任何色轮值改变之前原样写回这份 JSON。值一旦改变（`ApplyUpdate` 中任一字段变化），
  该 JSON 清除，之后写出 `version: 2`。
- 写入解析（`ParseColorWheelUpdate`）与 `LoadJson` 规则一致：没有 `version` 的 JSON 是 version 1
  完整 JSON（旧工程的 root、history 值、编辑前读到的 Model JSON），由 Model 转换；`version: 2`
  是新色轮的 patch；其他版本报错。所有新格式的写入方（面板、catalog、CLI）都写 `version: 2`。

## 5. 分阶段实施

### P1 — Qt-free 数学核心与 Model

- 新增 `edit/operators/models/color_wheel_tone.hpp/.cpp`：常量（`kToneBlackEv`、`kToneWhiteEv`、
  `kDiscMaxStops`、`kMasterMaxStops`）、色盘方向函数（从 `ui/.../color_wheel_math` 移入）、
  逐通道 kernel 参数计算、CPU 参考实现、旧格式转换函数。
- `ColorWheelModel`：offset 轮、`Offset()`、`ApplyUpdate` 支持 offset、JSON 版本与读入转换、
  新的 `IsIdentity`。
- 测试：JSON 往返；旧格式中性 → 新格式中性；旧 lift → offset 精确；带 gamma/gain 的旧格式
  在采样点上的误差低于 0.05 档；未编辑的旧 JSON 原样写回；编辑后写出 version 2；
  CPU 参考实现在不动点 ΔEV = 0、在标定点 ΔEV = K。

### P2 — 运行时打包与三个 GPU 后端

- `adjustment_runtime.cpp` 打包：`values[0..2]` offset（ACEScc 增量）、`[3..5]` lift 系数 K/R、
  `[6..8]` gamma 指数 p、`[9..11]` gain 系数 K/R、`[12]` 黑点 ACEScc、`[13]` 1/范围（ACEScc）。
- CUDA / OpenCL / Metal kernel 改为第 3 节的公式。
- 测试：各后端 grade 测试中色轮的期望像素值按 CPU 参考实现计算；`adjustment_runtime_test`
  检查打包布局。Metal 在 Mac 上构建和测试。

### P3 — 参数目录、写入解析与 CLI

- `editor_parameter_catalog.cpp`：增加 `offset_x/offset_y/offset_master`；`*_master` 以档为单位
  （`[−2, 2]`，step 0.01，2 位小数）；删除 `WheelSign`/`WheelBase`。
- `editor_parameter_write_parse.cpp`、`editor_parameter_write.cpp`、`editor_panel_projection`、
  `adjustment_transfer_catalog.cpp` 同步修改。
- `tests/edit/history/expected_serialized/*.json` 中的默认色轮序列化输出更新
  （新增 `version` 和 `offset`）。
- `*_master` 的数值单位改变，是 CLI 的不兼容变更，在 changelog 中写明。

### P4 — 面板、交互与文案

- `EditorCdlTrackballModel` / `EditorCdlTrackballItem`：增加 offset 轮；删除 gamma 的符号翻转和
  gamma/gain 的 unity 基准；Master 以档为单位；色轮下方显示逐通道档数。
- `EditorLookPanel.qml`：2×2 布局（暗部、中间调 / 高光、整体），标题和 tooltip：
  - 暗部 (Lift)："主要调整暗部的颜色和亮度。越接近黑色影响越大，高光基本不变。"
  - 中间调 (Gamma)："主要调整中间调。影响集中在中灰附近，最暗和最亮的部分基本不变。"
  - 高光 (Gain)："主要调整亮部的颜色和亮度。越接近白色影响越大，暗部基本不变。"
  - 整体 (Offset)："对所有影调做相同的颜色和亮度偏移，类似逐通道调整曝光。"
  - Master："调整该区域的亮度，单位为档（EV）。"
- `.ts` 文件手动编辑，不运行 lupdate。
- `editor_history_commit_presentation.cpp`：history 标签改为新名称，增加 offset。

### P5 — 验证与收尾

- Debug 构建，运行色轮相关的测试（不并行，不运行完整测试套件）。
- 发布说明在发布时用 `alcedo-release-notes` 编写，需要包含：色轮改为暗部/中间调/高光/整体
  四个轮且按档标定；旧工程打开时自动转换，未编辑前文件内容不变；CLI `*_master` 单位改为档，
  新增 `offset_*` 属性。

## 6. 已确认的决定

| # | 问题 | 决定 |
|---|---|---|
| 1 | 旧工程 | 不保留旧公式渲染分支；读入时转换为新解释（4.2） |
| 2 | 影调范围 | −6EV ~ +6EV，中灰在中点；固定，不开放 UI |
| 3 | 满幅力度 | 色盘 1.0 档，Master ±2.0 档 |
| 4 | CLI `*_master` 单位 | 改为档，在 changelog 中写明 |
| 5 | 布局 | 2×2（暗部、中间调 / 高光、整体） |

## 7. 完成记录（2026-10-18）

### 主要调用链

- 渲染：`ColorWheelModel` payload → `MakeGradeRuntimeParams`（`adjustment_runtime.cpp`）→
  `color_wheel::ResolveToneWheelKernelParams` → `values[0..13]` → `ApplyToneWheel`
  （CUDA `cuda_primary_grade_pass.cu`、OpenCL `primary_grade.cl`、Metal `primary_grade.metal`）。
  CPU 参考：`color_wheel::ApplyToneWheels`。Lift/Gamma/Gain 为中性时只加 Offset，结果逐位不变。
- 旧工程读入：`ColorGradeNodeModel` FromJson → `ColorWheelModel::LoadJson`（无 `version`）→
  `ConvertLegacyColorWheel` + `version_one_json`；`ToJson` 原样写回直到值改变。
- 面板：`EditorLookPanel.qml`（2×2）→ `EditorCdlTrackballModel`（拖动只写本轮）→
  `ColorWheelUpdate` → `ColorWheelModel::ApplyUpdate`。
- CLI / catalog：`ColorWheelUiToModel`（`version: 2`，`*_master` 单位为档）→
  `ParseColorWheelUpdate` → `ApplyUpdate`。

### 验证（Debug，Windows，CUDA + OpenCL）

- 运行的测试集：GpuDagModelGraphTest、PipelineDocumentCheckpointTest、GpuDagRawInputTest、
  GpuDagOpenClGradeTest、GpuDagCudaPrimaryGradeTest、EditorParameterCatalogTest、
  EditorPipelineCommandServiceTest、EditorLookModelTest、EditorCdlTrackballGeometryTest、
  EditorLookPanelInteractionTest、AdjustmentTransferCatalogTest、PipelineEditBatchTest：
  638 个中 620 个通过。
- 18 个失败均为既有问题：
  - 16 个 GPU multi-grade / neighbor / scene-work 测试在干净的 HEAD 上同样失败（已 stash 后重建验证）。
  - `ExistingRawRoot…` 与 `ExistingLmtRoot…`：新建文档与旧字节只差 HLS 默认值
    （`l_range`、`s_range`、`target_hls`，由 `d3f46808f`/#374 改动，未更新 fixture）。这两个测试中
    “解码旧 root/checkpoint 后重新编码字节一致”的断言通过，即旧工程的色轮字节与 root id 保持不变。
- 已重新生成新建文档的 3 个 fixture（`pipeline_root.json`、`pipeline_checkpoint.json`、
  `pipeline_document_multi_grade.json`），差异只有色轮格式、HLS 默认值与由此变化的 hash。
- 未验证：Metal（需在 Mac 上构建与运行 `metal_grade_test`）；GUI 中 2×2 布局与 tooltip 的实际显示。
